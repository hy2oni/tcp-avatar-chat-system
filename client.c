#include "common.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

#define UI_LOG_LINES 14
#define UI_LOG_LINES_COMPACT 3
#define UI_LOG_LINES_CHAT 12
#define UI_LINE_MAX 256
#define UI_MAP_MAX 4096
#define UI_COLOR_RESET "\033[0m"
#define UI_COLOR_TITLE "\033[1;36m"
#define UI_COLOR_ROOM "\033[1;33m"
#define UI_COLOR_SECTION "\033[1;34m"
#define UI_COLOR_WARN "\033[1;31m"
#define UI_COLOR_OK "\033[1;32m"
#define UI_COLOR_DIM "\033[2;37m"
#define UI_COLOR_PORTAL "\033[1;36m"
#define UI_COLOR_AVATAR1 "\033[1;32m"
#define UI_COLOR_AVATAR2 "\033[1;35m"
#define UI_COLOR_AVATAR3 "\033[1;34m"
#define UI_COLOR_AVATAR4 "\033[1;31m"
#define UI_COLOR_AVATAR5 "\033[1;33m"

typedef enum {
    MODE_LINE = 0,
    MODE_MOVE,
    MODE_CHAT,
    MODE_COMMAND
} InputMode;

static volatile int running = 1;
static pthread_mutex_t ui_mutex = PTHREAD_MUTEX_INITIALIZER;
static InputMode input_mode = MODE_LINE;
static char current_input[MAX_MSG];
static char latest_map[UI_MAP_MAX] = "Waiting for map update...";
static char latest_status[UI_LINE_MAX] = "Not logged in";
static char latest_notice[UI_LINE_MAX] = "";
static int latest_notice_ttl = 0;
static int current_room_id = 0;
static int quiz_visible = 0;
static int quiz_running = 0;
static int quiz_round = 0;
static int quiz_total = QUIZ_ROUNDS;
static int quiz_time_left = -1;
static char quiz_question[UI_LINE_MAX] = "No quiz running";
static char quiz_my_answer[16] = "-";
static char quiz_scores[UI_LINE_MAX] = "-";
static char quiz_result[UI_LINE_MAX] = "";
static char log_lines[UI_LOG_LINES][UI_LINE_MAX];
static int log_count = 0;
static int collecting_map = 0;
static char map_builder[UI_MAP_MAX];
static struct termios original_termios;
static int raw_terminal_enabled = 0;

static void restore_terminal(void) {
    if (raw_terminal_enabled) {
        tcsetattr(STDIN_FILENO, TCSANOW, &original_termios);
        raw_terminal_enabled = 0;
        printf("\033[?25h");
        fflush(stdout);
    }
}

static void handle_signal(int sig) {
    (void)sig;
    running = 0;
    restore_terminal();
    _exit(0);
}

static void trim_newline(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[n - 1] = '\0';
        n--;
    }
}

static void send_line(int sock, const char *line) {
    char buf[MAX_MSG + 4];
    snprintf(buf, sizeof(buf), "%s\n", line);
    send(sock, buf, strlen(buf), 0);
}

static const char *mode_name(InputMode mode) {
    switch (mode) {
        case MODE_MOVE:
            return "MOVE";
        case MODE_CHAT:
            return "CHAT";
        case MODE_COMMAND:
            return "COMMAND";
        default:
            return "LINE";
    }
}

static const char *mode_hint(InputMode mode) {
    switch (mode) {
        case MODE_MOVE:
            return "wasd instant | O/X quiz | c chat | / command | q quit";
        case MODE_CHAT:
            return "Enter sends chat | /move or ESC returns";
        case MODE_COMMAND:
            return "Enter runs command | ESC cancels";
        default:
            return "Enter sends command";
    }
}

static void ui_add_log_locked(const char *line) {
    if (line == NULL || line[0] == '\0' || strcmp(line, "LOGIN_OK") == 0) {
        return;
    }
    snprintf(log_lines[log_count % UI_LOG_LINES], UI_LINE_MAX, "%s", line);
    log_count++;
}

static void ui_set_notice_locked(const char *line) {
    snprintf(latest_notice, sizeof(latest_notice), "%s", line);
    latest_notice_ttl = 1;
}

static int is_system_notice(const char *line) {
    if (strncmp(line, "[Error]", 7) == 0 || strncmp(line, "[System]", 8) == 0) {
        return 1;
    }
    if (strstr(line, "entered the Lobby") || strstr(line, "left the Lobby")) {
        return 1;
    }
    if (strstr(line, "joined the table") || strstr(line, "left the table")) {
        return 1;
    }
    if (strstr(line, "You are on the Quiz Start Block") ||
        strstr(line, "You are the quiz starter") ||
        strstr(line, "/start is allowed only") ||
        strstr(line, "Answer saved") ||
        strstr(line, "not a participant")) {
        return 1;
    }
    return 0;
}

static int is_log_worthy(const char *line) {
    if (line == NULL || line[0] == '\0' || strcmp(line, "LOGIN_OK") == 0) {
        return 0;
    }
    if (strncmp(line, "WELCOME ", 8) == 0 ||
        strncmp(line, "Send LOGIN|", 11) == 0) {
        return 0;
    }
    if (strncmp(line, "[Help]", 6) == 0 ||
        strncmp(line, "w/a/s/d", 7) == 0 ||
        strncmp(line, "/chat", 5) == 0 ||
        strncmp(line, "/shout", 6) == 0 ||
        strncmp(line, "/map", 4) == 0 ||
        strncmp(line, "/start", 6) == 0 ||
        strncmp(line, "O or X", 6) == 0 ||
        strncmp(line, "/quit", 5) == 0) {
        return 0;
    }
    return !is_system_notice(line);
}

static void ui_extract_status_locked(const char *line) {
    if (strncmp(line, "You: ", 5) == 0) {
        snprintf(latest_status, sizeof(latest_status), "%s", line);
    }
}

static void ui_extract_room_locked(const char *line) {
    int room_id = 0;

    if (sscanf(line, "==== Room %d:", &room_id) == 1) {
        current_room_id = room_id;
        if (current_room_id != 2 && !quiz_running) {
            quiz_visible = 0;
        }
    }
}

// AI-assisted implementation: consumes compact quiz protocol lines for the fixed status panel.
static int ui_handle_quiz_protocol_locked(const char *line) {
    char copy[UI_LINE_MAX * 2];
    char *parts[5] = {0};
    char *save = NULL;
    char *token = NULL;
    int count = 0;

    if (strncmp(line, "QUIZ_", 5) != 0) {
        return 0;
    }

    if (strncmp(line, "QUIZ_SCORES|", 12) == 0) {
        quiz_visible = 1;
        snprintf(quiz_scores, sizeof(quiz_scores), "%s", line + 12);
        return 1;
    }
    if (strncmp(line, "QUIZ_RESULT|", 12) == 0) {
        quiz_visible = 1;
        snprintf(quiz_result, sizeof(quiz_result), "%s", line + 12);
        ui_add_log_locked("[Quiz] Round result updated.");
        return 1;
    }
    if (strncmp(line, "QUIZ_FINAL|", 11) == 0) {
        quiz_visible = 1;
        quiz_running = 0;
        quiz_time_left = 0;
        snprintf(quiz_result, sizeof(quiz_result), "%s", line + 11);
        ui_add_log_locked("[Quiz] Final scoreboard updated.");
        return 1;
    }

    snprintf(copy, sizeof(copy), "%s", line);
    token = strtok_r(copy, "|", &save);
    while (token != NULL && count < 5) {
        parts[count++] = token;
        token = strtok_r(NULL, "|", &save);
    }

    if (count == 0) {
        return 1;
    }
    if (strcmp(parts[0], "QUIZ_RESET") == 0) {
        quiz_visible = 1;
        quiz_running = 1;
        quiz_round = 0;
        quiz_total = count > 1 ? atoi(parts[1]) : QUIZ_ROUNDS;
        quiz_time_left = -1;
        snprintf(quiz_question, sizeof(quiz_question), "Waiting for first question");
        snprintf(quiz_my_answer, sizeof(quiz_my_answer), "-");
        snprintf(quiz_scores, sizeof(quiz_scores), "-");
        quiz_result[0] = '\0';
        return 1;
    }
    if (strcmp(parts[0], "QUIZ_STATE") == 0 && count >= 5) {
        quiz_visible = 1;
        quiz_running = 1;
        quiz_round = atoi(parts[1]);
        quiz_total = atoi(parts[2]);
        quiz_time_left = atoi(parts[3]);
        snprintf(quiz_question, sizeof(quiz_question), "%s", parts[4]);
        snprintf(quiz_my_answer, sizeof(quiz_my_answer), "-");
        quiz_result[0] = '\0';
        return 1;
    }
    if (strcmp(parts[0], "QUIZ_TICK") == 0 && count >= 2) {
        quiz_visible = 1;
        quiz_time_left = atoi(parts[1]);
        return 1;
    }
    if (strcmp(parts[0], "QUIZ_ANSWER") == 0 && count >= 2) {
        quiz_visible = 1;
        snprintf(quiz_my_answer, sizeof(quiz_my_answer), "%s", parts[1]);
        return 1;
    }
    return 1;
}

static const char *avatar_color(char ch) {
    switch (toupper((unsigned char)ch) % 5) {
        case 0:
            return UI_COLOR_AVATAR1;
        case 1:
            return UI_COLOR_AVATAR2;
        case 2:
            return UI_COLOR_AVATAR3;
        case 3:
            return UI_COLOR_AVATAR4;
        default:
            return UI_COLOR_AVATAR5;
    }
}

static void print_colored_map_text(const char *text) {
    int in_grid = 0;

    for (const char *p = text; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch == '\n') {
            putchar('\n');
            in_grid = 0;
        } else if (ch == '#') {
            in_grid = 1;
            printf(UI_COLOR_DIM "#");
        } else if (ch == '@') {
            printf(UI_COLOR_PORTAL "@");
        } else if (ch == 'Q') {
            printf(UI_COLOR_ROOM "Q");
        } else if (ch == 'T') {
            printf(UI_COLOR_WARN "T");
        } else if (in_grid && ch >= '1' && ch <= '4') {
            printf(UI_COLOR_PORTAL "%c", ch);
        } else if (in_grid && isalpha(ch)) {
            printf("%s%c", avatar_color((char)ch), ch);
        } else {
            printf(UI_COLOR_RESET "%c", ch);
        }
    }
    printf(UI_COLOR_RESET);
}

static const char *line_color(const char *line) {
    if (strstr(line, "[Error]") || strstr(line, "Wrong")) {
        return UI_COLOR_WARN;
    }
    if (strstr(line, "Correct") || strstr(line, "LOGIN_OK")) {
        return UI_COLOR_OK;
    }
    if (strstr(line, "[Quiz]") || strstr(line, "[Table")) {
        return UI_COLOR_ROOM;
    }
    return UI_COLOR_RESET;
}

static void print_trimmed_value(const char *label, const char *value) {
    char clipped[UI_LINE_MAX];

    snprintf(clipped, sizeof(clipped), "%s", value != NULL && value[0] != '\0' ? value : "-");
    if (strlen(clipped) > 60) {
        clipped[57] = '.';
        clipped[58] = '.';
        clipped[59] = '.';
        clipped[60] = '\0';
    }
    printf("%-8s %s\n", label, clipped);
}

static void ui_render_quiz_panel_locked(void) {
    if (!quiz_visible && current_room_id != 2) {
        return;
    }

    printf("-------------------------------------------------------------------\n");
    printf(UI_COLOR_SECTION "[ QUIZ STATUS ] " UI_COLOR_RESET);
    if (quiz_running) {
        printf(UI_COLOR_OK "RUNNING\n" UI_COLOR_RESET);
    } else {
        printf(UI_COLOR_DIM "READY\n" UI_COLOR_RESET);
    }
    printf("Round    %d / %d", quiz_round, quiz_total);
    if (quiz_time_left >= 0) {
        printf("        Time left: %02d sec", quiz_time_left);
    }
    printf("        My answer: %s\n", quiz_my_answer);
    print_trimmed_value("Question", quiz_question);
    print_trimmed_value("Scores", quiz_scores);
    if (quiz_result[0] != '\0') {
        print_trimmed_value("Result", quiz_result);
    }
}

// AI-assisted implementation: redraws a stable terminal dashboard without ncurses.
static void ui_render_locked(void) {
    int visible_logs = input_mode == MODE_CHAT ? UI_LOG_LINES_CHAT : UI_LOG_LINES_COMPACT;
    int start = log_count > visible_logs ? log_count - visible_logs : 0;

    printf("\033[2J\033[H");
    printf(UI_COLOR_TITLE "================ TCP Multi-room Avatar Chat System ================\n" UI_COLOR_RESET);
    printf(UI_COLOR_ROOM "                         Linux TCP / pthread demo\n" UI_COLOR_RESET);
    printf("-------------------------------------------------------------------\n");
    printf(UI_COLOR_SECTION "[ MAP / STATUS ]\n" UI_COLOR_RESET);
    print_colored_map_text(latest_map);
    printf("\n");
    ui_render_quiz_panel_locked();
    printf("-------------------------------------------------------------------\n");
    printf(UI_COLOR_SECTION "[ STATUS ] " UI_COLOR_RESET "%s\n", latest_status);
    if (latest_notice[0] != '\0') {
        printf(UI_COLOR_WARN "[ NOTICE ] %s\n" UI_COLOR_RESET, latest_notice);
    }
    printf("Mode: %s | %s\n", mode_name(input_mode), mode_hint(input_mode));
    printf("-------------------------------------------------------------------\n");
    printf(UI_COLOR_SECTION "%s\n" UI_COLOR_RESET,
           input_mode == MODE_CHAT ? "[ CHAT VIEW ]" : "[ RECENT LOG ]");
    for (int i = start; i < log_count; i++) {
        const char *line = log_lines[i % UI_LOG_LINES];
        printf("%s%s%s\n", line_color(line), line, UI_COLOR_RESET);
    }
    printf("-------------------------------------------------------------------\n");
    printf("%s > %s", mode_name(input_mode), current_input);
    fflush(stdout);

    if (latest_notice_ttl > 0) {
        latest_notice_ttl--;
        if (latest_notice_ttl == 0) {
            latest_notice[0] = '\0';
        }
    }
}

static void ui_handle_line_locked(const char *line) {
    if (ui_handle_quiz_protocol_locked(line)) {
        return;
    }

    if (is_system_notice(line)) {
        ui_set_notice_locked(line);
        return;
    }

    if (strncmp(line, "==== Room", 9) == 0) {
        collecting_map = 1;
        map_builder[0] = '\0';
        ui_extract_room_locked(line);
    }

    if (collecting_map) {
        strncat(map_builder, line, sizeof(map_builder) - strlen(map_builder) - 2);
        strncat(map_builder, "\n", sizeof(map_builder) - strlen(map_builder) - 1);
        ui_extract_status_locked(line);

        if (strncmp(line, "Commands:", 9) == 0) {
            collecting_map = 0;
            snprintf(latest_map, sizeof(latest_map), "%s", map_builder);
        }
        return;
    }

    if (strncmp(line, "Commands:", 9) == 0) {
        return;
    }
    if (is_log_worthy(line)) {
        ui_add_log_locked(line);
    }
}

// AI-assisted implementation: classifies incoming server text into map/status/log regions.
static void ui_process_server_text(const char *text) {
    char copy[4096];
    char *save = NULL;
    char *line = NULL;

    snprintf(copy, sizeof(copy), "%s", text);

    pthread_mutex_lock(&ui_mutex);
    line = strtok_r(copy, "\n", &save);
    while (line != NULL) {
        trim_newline(line);
        ui_handle_line_locked(line);
        line = strtok_r(NULL, "\n", &save);
    }
    ui_render_locked();
    pthread_mutex_unlock(&ui_mutex);
}

// AI-assisted implementation: continuously receives asynchronous messages and refreshes UI.
static void *recv_thread(void *arg) {
    int sock = *(int *)arg;
    char buf[2048];

    while (running) {
        ssize_t n = recv(sock, buf, sizeof(buf) - 1, 0);
        if (n == 0) {
            pthread_mutex_lock(&ui_mutex);
            ui_add_log_locked("[Client] Server closed the connection.");
            running = 0;
            ui_render_locked();
            pthread_mutex_unlock(&ui_mutex);
            break;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            running = 0;
            break;
        }
        buf[n] = '\0';
        ui_process_server_text(buf);
    }
    return NULL;
}

static int wait_for_login_result(int sock) {
    char buf[4096];
    char initial[UI_MAP_MAX * 2];
    int got_login_ok = 0;

    initial[0] = '\0';

    while (1) {
        ssize_t n;

        if (got_login_ok) {
            fd_set set;
            struct timeval tv;

            FD_ZERO(&set);
            FD_SET(sock, &set);
            tv.tv_sec = 1;
            tv.tv_usec = 0;

            if (select(sock + 1, &set, NULL, NULL, &tv) <= 0) {
                ui_process_server_text(initial);
                return 1;
            }
        }

        n = recv(sock, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
            return 0;
        }
        buf[n] = '\0';

        if (got_login_ok || strstr(buf, "LOGIN_OK") != NULL) {
            got_login_ok = 1;
            strncat(initial, buf, sizeof(initial) - strlen(initial) - 1);
            if (strstr(initial, "Commands:") != NULL) {
                ui_process_server_text(initial);
                return 1;
            }
            continue;
        }
        if (strstr(buf, "LOGIN_FAIL|") != NULL) {
            printf("%s", buf);
            fflush(stdout);
            return -1;
        }
    }
}

// AI-assisted implementation: repeats login until the server accepts unique nickname/avatar.
static int login_to_server(int sock) {
    char nick[MAX_NICK];
    char avatar[16];
    char login_msg[MAX_MSG];

    while (1) {
        printf("Nickname: ");
        fflush(stdout);
        if (!fgets(nick, sizeof(nick), stdin)) {
            return 0;
        }
        trim_newline(nick);

        printf("Avatar (single letter/number): ");
        fflush(stdout);
        if (!fgets(avatar, sizeof(avatar), stdin)) {
            return 0;
        }
        trim_newline(avatar);

        if (nick[0] == '\0' || strlen(avatar) != 1) {
            printf("[Client] Nickname is required and avatar must be one character.\n");
            continue;
        }

        snprintf(login_msg, sizeof(login_msg), "LOGIN|%s|%c", nick, avatar[0]);
        send_line(sock, login_msg);

        int result = wait_for_login_result(sock);
        if (result == 1) {
            return 1;
        }
        if (result == 0) {
            return 0;
        }
    }
}

static void normalize_input(char *line) {
    trim_newline(line);

    if (strcmp(line, "\033[A") == 0) {
        strcpy(line, "w");
    } else if (strcmp(line, "\033[B") == 0) {
        strcpy(line, "s");
    } else if (strcmp(line, "\033[D") == 0) {
        strcpy(line, "a");
    } else if (strcmp(line, "\033[C") == 0) {
        strcpy(line, "d");
    } else if (strlen(line) == 1) {
        line[0] = (char)tolower((unsigned char)line[0]);
    }
}

static int enable_raw_terminal(void) {
    struct termios raw;

    if (!isatty(STDIN_FILENO)) {
        return 0;
    }
    if (tcgetattr(STDIN_FILENO, &original_termios) != 0) {
        return 0;
    }

    raw = original_termios;
    raw.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
    raw.c_iflag &= (tcflag_t) ~(IXON | ICRNL);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
        return 0;
    }

    raw_terminal_enabled = 1;
    atexit(restore_terminal);
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    printf("\033[?25l");
    return 1;
}

static void ui_set_mode_locked(InputMode mode, const char *notice) {
    input_mode = mode;
    current_input[0] = '\0';
    if (notice != NULL) {
        ui_set_notice_locked(notice);
    }
}

static void ui_append_input_locked(char ch) {
    size_t len = strlen(current_input);
    if (len + 1 < sizeof(current_input)) {
        current_input[len] = ch;
        current_input[len + 1] = '\0';
    }
}

static void ui_backspace_input_locked(void) {
    size_t len = strlen(current_input);
    if (len > 0) {
        current_input[len - 1] = '\0';
    }
}

static void submit_chat_input(int sock) {
    char out[MAX_MSG + 16];

    pthread_mutex_lock(&ui_mutex);
    if (strcmp(current_input, "/move") == 0) {
        ui_set_mode_locked(MODE_MOVE, "[Client] Move mode.");
        ui_render_locked();
        pthread_mutex_unlock(&ui_mutex);
        return;
    }
    if (strcmp(current_input, "/quit") == 0 || strcmp(current_input, "/exit") == 0) {
        pthread_mutex_unlock(&ui_mutex);
        send_line(sock, current_input);
        running = 0;
        return;
    }
    if (current_input[0] == '/') {
        snprintf(out, sizeof(out), "%s", current_input);
    } else if (current_input[0] != '\0') {
        snprintf(out, sizeof(out), "/chat %s", current_input);
    } else {
        out[0] = '\0';
    }
    current_input[0] = '\0';
    ui_render_locked();
    pthread_mutex_unlock(&ui_mutex);

    if (out[0] != '\0') {
        send_line(sock, out);
    }
}

static void submit_command_input(int sock) {
    char out[MAX_MSG];

    pthread_mutex_lock(&ui_mutex);
    snprintf(out, sizeof(out), "%s", current_input);

    if (strcmp(out, "/chat") == 0 || strcmp(out, "/c") == 0) {
        ui_set_mode_locked(MODE_CHAT, "[Client] Chat mode. Type /move or ESC to return.");
        ui_render_locked();
        pthread_mutex_unlock(&ui_mutex);
        return;
    }

    ui_set_mode_locked(MODE_MOVE, NULL);
    ui_render_locked();
    pthread_mutex_unlock(&ui_mutex);

    if (out[0] != '\0') {
        send_line(sock, out);
        if (strcmp(out, "/quit") == 0 || strcmp(out, "/exit") == 0) {
            running = 0;
        }
    }
}

static void handle_escape_in_move_mode(int sock);

// AI-assisted implementation: raw terminal input loop for instant movement and modal chat.
static void raw_input_loop(int sock) {
    char ch;

    pthread_mutex_lock(&ui_mutex);
    ui_set_mode_locked(MODE_MOVE, "[Client] Move mode. WASD moves, O/X answers quizzes.");
    ui_render_locked();
    pthread_mutex_unlock(&ui_mutex);

    while (running && read(STDIN_FILENO, &ch, 1) == 1) {
        if (input_mode == MODE_MOVE) {
            char lower = (char)tolower((unsigned char)ch);
            if (ch == 27) {
                handle_escape_in_move_mode(sock);
            } else if (lower == 'w' || lower == 'a' || lower == 's' || lower == 'd') {
                char move[2] = {lower, '\0'};
                send_line(sock, move);
            } else if (lower == 'o' || lower == 'x') {
                char answer[2] = {(char)toupper((unsigned char)lower), '\0'};
                send_line(sock, answer);
            } else if (lower == 'c') {
                pthread_mutex_lock(&ui_mutex);
                ui_set_mode_locked(MODE_CHAT, "[Client] Chat mode. Type /move or ESC to return.");
                ui_render_locked();
                pthread_mutex_unlock(&ui_mutex);
            } else if (ch == '/') {
                pthread_mutex_lock(&ui_mutex);
                ui_set_mode_locked(MODE_COMMAND, NULL);
                strcpy(current_input, "/");
                ui_render_locked();
                pthread_mutex_unlock(&ui_mutex);
            } else if (lower == 'm') {
                send_line(sock, "/map");
            } else if (lower == 'h') {
                send_line(sock, "/help");
            } else if (lower == 'q') {
                send_line(sock, "/quit");
                running = 0;
                break;
            }
        } else {
            if (ch == 27) {
                pthread_mutex_lock(&ui_mutex);
                ui_set_mode_locked(MODE_MOVE, "[Client] Move mode.");
                ui_render_locked();
                pthread_mutex_unlock(&ui_mutex);
                continue;
            }
            if (ch == '\r' || ch == '\n') {
                if (input_mode == MODE_CHAT) {
                    submit_chat_input(sock);
                } else {
                    submit_command_input(sock);
                }
                continue;
            }
            if (ch == 127 || ch == '\b') {
                pthread_mutex_lock(&ui_mutex);
                ui_backspace_input_locked();
                ui_render_locked();
                pthread_mutex_unlock(&ui_mutex);
                continue;
            }
            if (isprint((unsigned char)ch)) {
                pthread_mutex_lock(&ui_mutex);
                ui_append_input_locked(ch);
                ui_render_locked();
                pthread_mutex_unlock(&ui_mutex);
            }
        }
    }
}

static void line_input_loop(int sock) {
    char line[MAX_MSG];

    while (running && fgets(line, sizeof(line), stdin)) {
        normalize_input(line);
        if (line[0] == '\0') {
            pthread_mutex_lock(&ui_mutex);
            ui_render_locked();
            pthread_mutex_unlock(&ui_mutex);
            continue;
        }
        send_line(sock, line);
        if (strcmp(line, "/quit") == 0 || strcmp(line, "/exit") == 0) {
            running = 0;
            break;
        }
    }
}

static int read_byte_with_timeout(char *out) {
    fd_set set;
    struct timeval tv;

    FD_ZERO(&set);
    FD_SET(STDIN_FILENO, &set);
    tv.tv_sec = 0;
    tv.tv_usec = 30000;

    if (select(STDIN_FILENO + 1, &set, NULL, NULL, &tv) > 0) {
        return read(STDIN_FILENO, out, 1) == 1;
    }
    return 0;
}

static void handle_escape_in_move_mode(int sock) {
    char seq1;
    char seq2;
    char move[2] = {'\0', '\0'};

    if (!read_byte_with_timeout(&seq1) || !read_byte_with_timeout(&seq2)) {
        return;
    }
    if (seq1 != '[') {
        return;
    }

    if (seq2 == 'A') {
        move[0] = 'w';
    } else if (seq2 == 'B') {
        move[0] = 's';
    } else if (seq2 == 'C') {
        move[0] = 'd';
    } else if (seq2 == 'D') {
        move[0] = 'a';
    }

    if (move[0] != '\0') {
        send_line(sock, move);
    }
}

// AI-assisted implementation: Linux TCP client with dashboard UI and input sender.
int main(int argc, char *argv[]) {
    const char *host = "127.0.0.1";
    int port = DEFAULT_PORT;
    int sock;
    struct sockaddr_in server_addr;

    if (argc >= 2) {
        host = argv[1];
    }
    if (argc >= 3) {
        port = atoi(argv[2]);
    }

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons((uint16_t)port);

    if (inet_pton(AF_INET, host, &server_addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(sock);
        return 1;
    }

    if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        close(sock);
        return 1;
    }

    printf("[Client] Connected to %s:%d\n", host, port);
    if (!login_to_server(sock)) {
        close(sock);
        return 1;
    }

    pthread_t tid;
    if (pthread_create(&tid, NULL, recv_thread, &sock) != 0) {
        perror("pthread_create");
        close(sock);
        return 1;
    }

    pthread_mutex_lock(&ui_mutex);
    ui_add_log_locked("[Client] MOVE: wasd instant, O/X quiz answer, c chat, / command.");
    pthread_mutex_unlock(&ui_mutex);

    if (enable_raw_terminal()) {
        raw_input_loop(sock);
    } else {
        pthread_mutex_lock(&ui_mutex);
        ui_set_mode_locked(MODE_LINE, "[Client] Line input mode. Press Enter after each command.");
        ui_render_locked();
        pthread_mutex_unlock(&ui_mutex);
        line_input_loop(sock);
    }

    restore_terminal();
    shutdown(sock, SHUT_RDWR);
    close(sock);
    pthread_join(tid, NULL);
    printf("\n");
    return 0;
}
