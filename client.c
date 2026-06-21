#include "common.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define UI_LOG_LINES 14
#define UI_LINE_MAX 256
#define UI_MAP_MAX 4096
#define UI_COLOR_RESET "\033[0m"
#define UI_COLOR_TITLE "\033[1;36m"
#define UI_COLOR_ROOM "\033[1;33m"
#define UI_COLOR_SECTION "\033[1;34m"
#define UI_COLOR_WARN "\033[1;31m"
#define UI_COLOR_OK "\033[1;32m"

static volatile int running = 1;
static pthread_mutex_t ui_mutex = PTHREAD_MUTEX_INITIALIZER;
static char latest_map[UI_MAP_MAX] = "Waiting for map update...";
static char latest_status[UI_LINE_MAX] = "Not logged in";
static char log_lines[UI_LOG_LINES][UI_LINE_MAX];
static int log_count = 0;
static int collecting_map = 0;
static char map_builder[UI_MAP_MAX];

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

static void ui_add_log_locked(const char *line) {
    if (line == NULL || line[0] == '\0' || strcmp(line, "LOGIN_OK") == 0) {
        return;
    }
    snprintf(log_lines[log_count % UI_LOG_LINES], UI_LINE_MAX, "%s", line);
    log_count++;
}

static void ui_extract_status_locked(const char *line) {
    if (strncmp(line, "You: ", 5) == 0) {
        snprintf(latest_status, sizeof(latest_status), "%s", line);
    }
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

// AI-assisted implementation: redraws a stable terminal dashboard without ncurses.
static void ui_render_locked(void) {
    int start = log_count > UI_LOG_LINES ? log_count - UI_LOG_LINES : 0;

    printf("\033[2J\033[H");
    printf(UI_COLOR_TITLE "================ TCP Multi-room Avatar Chat System ================\n" UI_COLOR_RESET);
    printf(UI_COLOR_ROOM "                         Linux TCP / pthread demo\n" UI_COLOR_RESET);
    printf("-------------------------------------------------------------------\n");
    printf(UI_COLOR_SECTION "[ MAP / STATUS ]\n" UI_COLOR_RESET);
    printf("%s\n", latest_map);
    printf("-------------------------------------------------------------------\n");
    printf(UI_COLOR_SECTION "[ STATUS ] " UI_COLOR_RESET "%s\n", latest_status);
    printf("Commands: w/a/s/d | /chat msg | /shout msg | /map | /help | /quit\n");
    printf("-------------------------------------------------------------------\n");
    printf(UI_COLOR_SECTION "[ CHAT / SYSTEM / QUIZ LOG ]\n" UI_COLOR_RESET);
    for (int i = start; i < log_count; i++) {
        const char *line = log_lines[i % UI_LOG_LINES];
        printf("%s%s%s\n", line_color(line), line, UI_COLOR_RESET);
    }
    printf("-------------------------------------------------------------------\n");
    printf("Input > ");
    fflush(stdout);
}

static void ui_handle_line_locked(const char *line) {
    if (strncmp(line, "==== Room", 9) == 0) {
        collecting_map = 1;
        map_builder[0] = '\0';
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
    ui_add_log_locked(line);
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

    while (1) {
        ssize_t n = recv(sock, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
            return 0;
        }
        buf[n] = '\0';
        printf("%s", buf);
        fflush(stdout);

        if (strstr(buf, "LOGIN_OK") != NULL) {
            return 1;
        }
        if (strstr(buf, "LOGIN_FAIL|") != NULL) {
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
    ui_add_log_locked("[Client] Type /help for commands. WASD is the stable movement input.");
    ui_render_locked();
    pthread_mutex_unlock(&ui_mutex);

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

    shutdown(sock, SHUT_RDWR);
    close(sock);
    pthread_join(tid, NULL);
    printf("\n");
    return 0;
}
