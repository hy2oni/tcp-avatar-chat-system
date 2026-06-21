#include "common.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    int sock;
    int active;
    char nickname[MAX_NICK];
    char avatar;
    int room_id;
    int x;
    int y;
    int quiz_participant;
    int quiz_score;
    char quiz_answer;
    int has_answered;
    int table_id;
} Client;

typedef struct {
    int id;
    char name[64];
    int width;
    int height;
    char base_map[MAP_H][MAP_W + 1];
} Room;

typedef struct {
    int from_room;
    int x;
    int y;
    int to_room;
    int dest_x;
    int dest_y;
} Portal;

typedef struct {
    char question[256];
    char answer;
} QuizQuestion;

typedef struct {
    int running;
    int starter_sock;
    int starter_set;
    int current_round;
    int question_indices[QUIZ_ROUNDS];
} QuizState;

typedef struct {
    int table_id;
    char logs[MAX_TABLE_LOGS][MAX_MSG];
    int log_count;
} TableState;

static Client clients[MAX_CLIENTS];
static Room rooms[MAX_ROOMS + 1];
static Portal portals[] = {
    {1, 7, 3, 2, 14, 9},
    {2, 14, 10, 1, 7, 4},
    {1, 22, 3, 3, 14, 9},
    {3, 14, 10, 1, 22, 4},
};
static QuizQuestion questions[MAX_QUESTIONS];
static QuizState quiz_state;
static TableState tables[TABLE_COUNT + 1];

static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t quiz_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t table_mutex = PTHREAD_MUTEX_INITIALIZER;

static void send_to_client(int sock, const char *msg);
static void send_map_to_client_locked(int idx);
static void broadcast_maps_to_room_locked(int room_id);
static void handle_tile_event_after_move(int idx);
static void disconnect_client(int idx);

static void server_log(const char *fmt, ...) {
    va_list args;
    char timebuf[32];
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);

    strftime(timebuf, sizeof(timebuf), "%H:%M:%S", tm_info);
    printf("[%s] ", timebuf);
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
    fflush(stdout);
}

static void trim_newline(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[n - 1] = '\0';
        n--;
    }
}

static int recv_line(int sock, char *buf, size_t size) {
    size_t pos = 0;
    while (pos + 1 < size) {
        char ch;
        ssize_t n = recv(sock, &ch, 1, 0);
        if (n == 0) {
            return 0;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (ch == '\n') {
            break;
        }
        buf[pos++] = ch;
    }
    buf[pos] = '\0';
    trim_newline(buf);
    return (int)pos;
}

static void send_to_client(int sock, const char *msg) {
    if (sock < 0 || msg == NULL) {
        return;
    }
    send(sock, msg, strlen(msg), MSG_NOSIGNAL);
}

static void init_rooms(void) {
    rooms[1].id = 1;
    strcpy(rooms[1].name, "Lobby");
    rooms[1].width = MAP_W;
    rooms[1].height = MAP_H;
    const char *lobby[MAP_H] = {
        "##############################",
        "#.............##.............#",
        "#......2......##......3......#",
        "#......@......##......@......#",
        "#.............##.............#",
        "#............................#",
        "#............................#",
        "#............................#",
        "#............................#",
        "#............................#",
        "#............................#",
        "##############################",
    };

    rooms[2].id = 2;
    strcpy(rooms[2].name, "O/X Quiz Room");
    rooms[2].width = MAP_W;
    rooms[2].height = MAP_H;
    const char *quiz[MAP_H] = {
        "##############################",
        "#............................#",
        "#............................#",
        "#.....####..........####.....#",
        "#............................#",
        "#.............Q..............#",
        "#............................#",
        "#.....####..........####.....#",
        "#............................#",
        "#............................#",
        "#.............@..............#",
        "##############################",
    };

    rooms[3].id = 3;
    strcpy(rooms[3].name, "Private Table Room");
    rooms[3].width = MAP_W;
    rooms[3].height = MAP_H;
    const char *table_room[MAP_H] = {
        "##############################",
        "#............................#",
        "#.....1................2.....#",
        "#....1T1..............2T2....#",
        "#.....1................2.....#",
        "#............................#",
        "#.....3................4.....#",
        "#....3T3..............4T4....#",
        "#.....3................4.....#",
        "#............................#",
        "#.............@..............#",
        "##############################",
    };

    for (int y = 0; y < MAP_H; y++) {
        strncpy(rooms[1].base_map[y], lobby[y], MAP_W + 1);
        strncpy(rooms[2].base_map[y], quiz[y], MAP_W + 1);
        strncpy(rooms[3].base_map[y], table_room[y], MAP_W + 1);
    }
}

static void init_quiz_questions(void) {
    QuizQuestion pool[MAX_QUESTIONS] = {
        {"TCP is a connection-oriented protocol.", 'O'},
        {"UDP guarantees reliable delivery.", 'X'},
        {"The socket() function creates a socket descriptor.", 'O'},
        {"bind() is used only by clients and never by servers.", 'X'},
        {"accept() is used by a TCP server to accept a client connection.", 'O'},
        {"listen() puts a TCP socket into passive listening mode.", 'O'},
        {"IP addresses identify hosts at the network layer.", 'O'},
        {"A port number helps identify a process or service on a host.", 'O'},
        {"TCP and UDP both use three-way handshake before sending data.", 'X'},
        {"recv() can receive data from a connected TCP socket.", 'O'},
    };

    memcpy(questions, pool, sizeof(pool));
    memset(&quiz_state, 0, sizeof(quiz_state));
    quiz_state.current_round = -1;
}

static void init_tables(void) {
    for (int i = 1; i <= TABLE_COUNT; i++) {
        tables[i].table_id = i;
        tables[i].log_count = 0;
    }
}

static int nickname_available_locked(const char *nickname) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && strcmp(clients[i].nickname, nickname) == 0) {
            return 0;
        }
    }
    return 1;
}

static int avatar_available_locked(char avatar) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].avatar == avatar) {
            return 0;
        }
    }
    return 1;
}

static int is_occupied_locked(int room_id, int x, int y, int exclude_idx) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].active || i == exclude_idx) {
            continue;
        }
        if (clients[i].room_id == room_id && clients[i].x == x && clients[i].y == y) {
            return 1;
        }
    }
    return 0;
}

static int is_walkable_tile(int room_id, int x, int y) {
    if (room_id < 1 || room_id > MAX_ROOMS || x < 0 || y < 0 || x >= MAP_W || y >= MAP_H) {
        return 0;
    }

    char tile = rooms[room_id].base_map[y][x];
    if (tile == '.' || tile == '@' || tile == 'Q') {
        return 1;
    }
    if (room_id == 3 && tile >= '1' && tile <= '4') {
        return 1;
    }
    return 0;
}

static int is_spawnable_locked(int room_id, int x, int y) {
    if (room_id < 1 || room_id > MAX_ROOMS || x < 0 || y < 0 || x >= MAP_W || y >= MAP_H) {
        return 0;
    }
    return rooms[room_id].base_map[y][x] == '.' && !is_occupied_locked(room_id, x, y, -1);
}

static int find_random_spawn_locked(int room_id, int *out_x, int *out_y) {
    if (room_id == 1) {
        int candidates[15][2];
        int count = 0;

        for (int y = 7; y <= 9; y++) {
            for (int x = 12; x <= 16; x++) {
                if (is_spawnable_locked(room_id, x, y)) {
                    candidates[count][0] = x;
                    candidates[count][1] = y;
                    count++;
                }
            }
        }

        if (count > 0) {
            int pick = rand() % count;
            *out_x = candidates[pick][0];
            *out_y = candidates[pick][1];
            return 1;
        }
    }

    for (int tries = 0; tries < 500; tries++) {
        int x = rand() % MAP_W;
        int y = rand() % MAP_H;
        if (is_spawnable_locked(room_id, x, y)) {
            *out_x = x;
            *out_y = y;
            return 1;
        }
    }

    for (int y = 0; y < MAP_H; y++) {
        for (int x = 0; x < MAP_W; x++) {
            if (is_spawnable_locked(room_id, x, y)) {
                *out_x = x;
                *out_y = y;
                return 1;
            }
        }
    }
    return 0;
}

// AI-assisted implementation: finds a nearby empty floor when a portal destination is occupied.
static int find_nearby_spawn_locked(int room_id, int center_x, int center_y, int *out_x, int *out_y) {
    static const int preferred[][2] = {
        {0, 0}, {0, 1}, {-1, 0}, {1, 0}, {0, -1},
        {-1, 1}, {1, 1}, {-1, -1}, {1, -1},
        {0, 2}, {-2, 0}, {2, 0}, {0, -2},
        {-1, 2}, {1, 2}, {-2, 1}, {2, 1},
        {-2, -1}, {2, -1}, {-1, -2}, {1, -2},
        {0, 3}, {-3, 0}, {3, 0}, {0, -3}
    };

    for (size_t i = 0; i < sizeof(preferred) / sizeof(preferred[0]); i++) {
        int x = center_x + preferred[i][0];
        int y = center_y + preferred[i][1];
        if (is_spawnable_locked(room_id, x, y)) {
            *out_x = x;
            *out_y = y;
            return 1;
        }
    }

    for (int radius = 4; radius <= 6; radius++) {
        for (int dy = -radius; dy <= radius; dy++) {
            for (int dx = -radius; dx <= radius; dx++) {
                if (abs(dx) + abs(dy) != radius) {
                    continue;
                }
                int x = center_x + dx;
                int y = center_y + dy;
                if (is_spawnable_locked(room_id, x, y)) {
                    *out_x = x;
                    *out_y = y;
                    return 1;
                }
            }
        }
    }

    return 0;
}

static int table_id_for_tile(int room_id, int x, int y) {
    if (room_id != 3 || x < 0 || y < 0 || x >= MAP_W || y >= MAP_H) {
        return 0;
    }
    char tile = rooms[3].base_map[y][x];
    if (tile >= '1' && tile <= '4') {
        return tile - '0';
    }
    return 0;
}

static int table_member_count_locked(int table_id) {
    int count = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].room_id == 3 && clients[i].table_id == table_id) {
            count++;
        }
    }
    return count;
}

static void broadcast_room_locked(int room_id, const char *msg) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].room_id == room_id) {
            send_to_client(clients[i].sock, msg);
        }
    }
}

static void broadcast_table_locked(int table_id, const char *msg) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].room_id == 3 && clients[i].table_id == table_id) {
            send_to_client(clients[i].sock, msg);
        }
    }
}

static void broadcast_quiz_viewers_locked(const char *msg) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].active) {
            continue;
        }
        if (clients[i].room_id == 2 || clients[i].quiz_participant) {
            send_to_client(clients[i].sock, msg);
        }
    }
}

// AI-assisted implementation: renders authoritative server map with dynamic client avatars.
static void render_map_for_client_locked(int idx, char *out, size_t outsz) {
    char map[MAP_H][MAP_W + 1];
    int room_id = clients[idx].room_id;
    size_t used = 0;

    for (int y = 0; y < MAP_H; y++) {
        strncpy(map[y], rooms[room_id].base_map[y], MAP_W + 1);
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].room_id == room_id) {
            map[clients[i].y][clients[i].x] = clients[i].avatar;
        }
    }

    used += snprintf(out + used, outsz - used,
                     "\n==== Room %d: %s ====\nYou: %s(%c) at (%d,%d)\n",
                     room_id, rooms[room_id].name, clients[idx].nickname,
                     clients[idx].avatar, clients[idx].x, clients[idx].y);
    for (int y = 0; y < MAP_H && used < outsz; y++) {
        used += snprintf(out + used, outsz - used, "%s\n", map[y]);
    }
    snprintf(out + used, outsz - used, "Commands: w/a/s/d, /chat, /map, /help, /quit\n\n");
}

static void send_map_to_client_locked(int idx) {
    char out[4096];
    render_map_for_client_locked(idx, out, sizeof(out));
    send_to_client(clients[idx].sock, out);
}

// AI-assisted implementation: pushes fresh room maps to all clients after shared state changes.
static void broadcast_maps_to_room_locked(int room_id) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active && clients[i].room_id == room_id) {
            send_map_to_client_locked(i);
        }
    }
}

static void send_help(int sock) {
    const char *help =
        "\n[Help]\n"
        "w/a/s/d or arrow keys: move\n"
        "/chat <message>, /c <message>: room chat or table chat\n"
        "/shout <message>, /s <message>: public Room 3 chat\n"
        "/map: print current map\n"
        "/start: start O/X quiz when allowed in Room 2\n"
        "O or X: answer quiz question while participating\n"
        "/quit or /exit: disconnect\n\n";
    send_to_client(sock, help);
}

static void add_table_log(int table_id, const char *msg) {
    pthread_mutex_lock(&table_mutex);
    if (table_id >= 1 && table_id <= TABLE_COUNT) {
        int pos = tables[table_id].log_count % MAX_TABLE_LOGS;
        snprintf(tables[table_id].logs[pos], MAX_MSG, "%s", msg);
        tables[table_id].log_count++;
    }
    pthread_mutex_unlock(&table_mutex);
}

static void clear_table_log(int table_id) {
    pthread_mutex_lock(&table_mutex);
    if (table_id >= 1 && table_id <= TABLE_COUNT) {
        tables[table_id].log_count = 0;
        memset(tables[table_id].logs, 0, sizeof(tables[table_id].logs));
    }
    pthread_mutex_unlock(&table_mutex);
}

// AI-assisted implementation: keeps table membership synchronized with seat tiles.
static void update_table_membership_after_move_locked(int idx, int old_table) {
    int new_table = table_id_for_tile(clients[idx].room_id, clients[idx].x, clients[idx].y);
    char msg[MAX_MSG];

    clients[idx].table_id = new_table;
    if (old_table == new_table) {
        return;
    }

    if (old_table > 0) {
        snprintf(msg, sizeof(msg), "[Table %d] %s left the table.\n", old_table, clients[idx].nickname);
        broadcast_table_locked(old_table, msg);
        send_to_client(clients[idx].sock, "[System] You left the private table.\n");
        if (table_member_count_locked(old_table) == 0) {
            clear_table_log(old_table);
            server_log("[TABLE] Table %d empty. Private chat logs cleared.", old_table);
        }
    }

    if (new_table > 0) {
        snprintf(msg, sizeof(msg), "[Table %d] %s joined the table.\n", new_table, clients[idx].nickname);
        broadcast_table_locked(new_table, msg);
        snprintf(msg, sizeof(msg), "[System] You joined Table %d. /chat is private here; /shout is public.\n", new_table);
        send_to_client(clients[idx].sock, msg);
        server_log("[TABLE] %s joined Table %d.", clients[idx].nickname, new_table);
    }
}

// AI-assisted implementation: enforces fixed portal transitions and safe destination coordinates.
static void handle_portal_if_any_locked(int idx) {
    int old_room = clients[idx].room_id;
    int old_table = clients[idx].table_id;

    for (size_t i = 0; i < sizeof(portals) / sizeof(portals[0]); i++) {
        Portal *p = &portals[i];
        if (clients[idx].room_id == p->from_room && clients[idx].x == p->x && clients[idx].y == p->y) {
            int dest_x = p->dest_x;
            int dest_y = p->dest_y;
            char msg[MAX_MSG];

            if (!is_spawnable_locked(p->to_room, dest_x, dest_y) &&
                !find_nearby_spawn_locked(p->to_room, dest_x, dest_y, &dest_x, &dest_y)) {
                find_random_spawn_locked(p->to_room, &dest_x, &dest_y);
            }

            clients[idx].room_id = p->to_room;
            clients[idx].x = dest_x;
            clients[idx].y = dest_y;
            clients[idx].table_id = 0;

            if (old_table > 0 && table_member_count_locked(old_table) == 0) {
                clear_table_log(old_table);
                server_log("[TABLE] Table %d empty. Private chat logs cleared.", old_table);
            }

            snprintf(msg, sizeof(msg), "[System] Moved to Room %d: %s.\n", p->to_room, rooms[p->to_room].name);
            send_to_client(clients[idx].sock, msg);
            server_log("[PORTAL] %s moved Room %d -> Room %d.", clients[idx].nickname, old_room, p->to_room);

            if (old_room == 1) {
                snprintf(msg, sizeof(msg), "[Lobby] %s left the Lobby.\n", clients[idx].nickname);
                broadcast_room_locked(1, msg);
            } else if (p->to_room == 1) {
                snprintf(msg, sizeof(msg), "[Lobby] %s entered the Lobby.\n", clients[idx].nickname);
                broadcast_room_locked(1, msg);
            }
            return;
        }
    }
}

static int client_on_quiz_block_locked(int idx) {
    return clients[idx].room_id == 2 && rooms[2].base_map[clients[idx].y][clients[idx].x] == 'Q';
}

static void handle_tile_event_after_move(int idx) {
    int sock;
    int on_q;

    pthread_mutex_lock(&clients_mutex);
    sock = clients[idx].sock;
    on_q = client_on_quiz_block_locked(idx);
    pthread_mutex_unlock(&clients_mutex);

    if (!on_q) {
        return;
    }

    send_to_client(sock, "[Quiz] You are on the Quiz Start Block. First visitor may type /start to start.\n");

    pthread_mutex_lock(&quiz_mutex);
    pthread_mutex_lock(&clients_mutex);
    if (!quiz_state.running && !quiz_state.starter_set && clients[idx].active && client_on_quiz_block_locked(idx)) {
        quiz_state.starter_sock = clients[idx].sock;
        quiz_state.starter_set = 1;
        send_to_client(clients[idx].sock, "[Quiz] You are the quiz starter. Type /start while standing on Q.\n");
        server_log("[QUIZ] %s became quiz starter.", clients[idx].nickname);
    }
    pthread_mutex_unlock(&clients_mutex);
    pthread_mutex_unlock(&quiz_mutex);
}

// AI-assisted implementation: validates movement, collisions, portals, and table seats.
static void handle_move(int idx, int dx, int dy) {
    int moved = 0;
    int old_table = 0;
    int sock = -1;
    int old_room = 0;
    int new_room = 0;

    pthread_mutex_lock(&clients_mutex);
    if (!clients[idx].active) {
        pthread_mutex_unlock(&clients_mutex);
        return;
    }

    int nx = clients[idx].x + dx;
    int ny = clients[idx].y + dy;
    sock = clients[idx].sock;
    old_table = clients[idx].table_id;
    old_room = clients[idx].room_id;

    if (!is_walkable_tile(clients[idx].room_id, nx, ny)) {
        send_to_client(sock, "[Error] You cannot move there.\n");
    } else if (is_occupied_locked(clients[idx].room_id, nx, ny, idx)) {
        send_to_client(sock, "[Error] That tile is already occupied.\n");
    } else {
        clients[idx].x = nx;
        clients[idx].y = ny;
        moved = 1;
        update_table_membership_after_move_locked(idx, old_table);
        handle_portal_if_any_locked(idx);
        new_room = clients[idx].room_id;
        server_log("[MOVE] %s -> Room %d (%d,%d).", clients[idx].nickname,
                   clients[idx].room_id, clients[idx].x, clients[idx].y);
        broadcast_maps_to_room_locked(old_room);
        if (new_room != old_room) {
            broadcast_maps_to_room_locked(new_room);
        }
    }
    pthread_mutex_unlock(&clients_mutex);

    if (moved) {
        handle_tile_event_after_move(idx);
    }
}

static void handle_chat(int idx, const char *message) {
    char out[MAX_MSG + MAX_NICK + 64];

    pthread_mutex_lock(&clients_mutex);
    if (!clients[idx].active) {
        pthread_mutex_unlock(&clients_mutex);
        return;
    }

    if (clients[idx].room_id == 3 && clients[idx].table_id > 0) {
        snprintf(out, sizeof(out), "[Table %d] %s: %s\n", clients[idx].table_id,
                 clients[idx].nickname, message);
        broadcast_table_locked(clients[idx].table_id, out);
        add_table_log(clients[idx].table_id, out);
        server_log("[TABLE CHAT] Table %d %s: %s", clients[idx].table_id, clients[idx].nickname, message);
    } else {
        snprintf(out, sizeof(out), "[%s] %s: %s\n", rooms[clients[idx].room_id].name,
                 clients[idx].nickname, message);
        broadcast_room_locked(clients[idx].room_id, out);
        server_log("[CHAT] Room %d %s: %s", clients[idx].room_id, clients[idx].nickname, message);
    }

    pthread_mutex_unlock(&clients_mutex);
}

static void handle_shout(int idx, const char *message) {
    char out[MAX_MSG + MAX_NICK + 64];

    pthread_mutex_lock(&clients_mutex);
    if (clients[idx].active) {
        snprintf(out, sizeof(out), "[%s Public] %s: %s\n", rooms[clients[idx].room_id].name,
                 clients[idx].nickname, message);
        broadcast_room_locked(clients[idx].room_id, out);
        server_log("[SHOUT] Room %d %s: %s", clients[idx].room_id, clients[idx].nickname, message);
    }
    pthread_mutex_unlock(&clients_mutex);
}

static void shuffle_question_indices(int out[QUIZ_ROUNDS]) {
    int all[MAX_QUESTIONS];
    for (int i = 0; i < MAX_QUESTIONS; i++) {
        all[i] = i;
    }
    for (int i = MAX_QUESTIONS - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int tmp = all[i];
        all[i] = all[j];
        all[j] = tmp;
    }
    for (int i = 0; i < QUIZ_ROUNDS; i++) {
        out[i] = all[i];
    }
}

// AI-assisted implementation: timer-driven O/X quiz loop with final scoreboard.
static void *quiz_thread(void *arg) {
    (void)arg;

    for (int round = 0; round < QUIZ_ROUNDS; round++) {
        int qidx;
        char msg[4096];

        pthread_mutex_lock(&quiz_mutex);
        quiz_state.current_round = round;
        qidx = quiz_state.question_indices[round];
        pthread_mutex_unlock(&quiz_mutex);

        pthread_mutex_lock(&clients_mutex);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].active && clients[i].quiz_participant) {
                clients[i].quiz_answer = '-';
                clients[i].has_answered = 0;
            }
        }
        snprintf(msg, sizeof(msg),
                 "\n[Quiz] Round %d/%d - 10 seconds\n[Quiz] %s (Answer with O or X)\n",
                 round + 1, QUIZ_ROUNDS, questions[qidx].question);
        broadcast_quiz_viewers_locked(msg);
        pthread_mutex_unlock(&clients_mutex);

        server_log("[QUIZ] Round %d started.", round + 1);
        sleep(10);

        pthread_mutex_lock(&clients_mutex);
        size_t used = 0;
        used += snprintf(msg + used, sizeof(msg) - used,
                         "\n[Quiz] Time up. Correct answer: %c\n", questions[qidx].answer);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!clients[i].active || !clients[i].quiz_participant) {
                continue;
            }
            const char *status = "No answer";
            if (clients[i].has_answered) {
                if (clients[i].quiz_answer == questions[qidx].answer) {
                    clients[i].quiz_score++;
                    status = "Correct";
                } else {
                    status = "Wrong";
                }
            }
            used += snprintf(msg + used, sizeof(msg) - used,
                             "[Quiz] %-16s answer=%c result=%s score=%d\n",
                             clients[i].nickname, clients[i].quiz_answer, status, clients[i].quiz_score);
        }
        used += snprintf(msg + used, sizeof(msg) - used, "\n");
        broadcast_quiz_viewers_locked(msg);
        pthread_mutex_unlock(&clients_mutex);
    }

    pthread_mutex_lock(&clients_mutex);
    char final[4096];
    size_t used = 0;
    int top_score = -1;
    int low_score = 9999;
    int participant_count = 0;

    used += snprintf(final + used, sizeof(final) - used, "\n[Quiz] Final scoreboard\n");
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].active || !clients[i].quiz_participant) {
            continue;
        }
        participant_count++;
        if (clients[i].quiz_score > top_score) {
            top_score = clients[i].quiz_score;
        }
        if (clients[i].quiz_score < low_score) {
            low_score = clients[i].quiz_score;
        }
        used += snprintf(final + used, sizeof(final) - used,
                         "[Quiz] %-16s %d/%d\n", clients[i].nickname,
                         clients[i].quiz_score, QUIZ_ROUNDS);
    }

    if (participant_count == 0) {
        used += snprintf(final + used, sizeof(final) - used, "[Quiz] No active participants.\n");
    } else {
        used += snprintf(final + used, sizeof(final) - used, "[Quiz] Winner(s): ");
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].active && clients[i].quiz_participant && clients[i].quiz_score == top_score) {
                used += snprintf(final + used, sizeof(final) - used, "%s ", clients[i].nickname);
            }
        }
        used += snprintf(final + used, sizeof(final) - used, "\n[Quiz] Loser(s): ");
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].active && clients[i].quiz_participant && clients[i].quiz_score == low_score) {
                used += snprintf(final + used, sizeof(final) - used, "%s ", clients[i].nickname);
            }
        }
        if (top_score == low_score) {
            used += snprintf(final + used, sizeof(final) - used, "\n[Quiz] Draw: all players have the same score.");
        }
        used += snprintf(final + used, sizeof(final) - used, "\n\n");
    }
    broadcast_quiz_viewers_locked(final);
    pthread_mutex_unlock(&clients_mutex);

    pthread_mutex_lock(&quiz_mutex);
    quiz_state.running = 0;
    quiz_state.starter_set = 0;
    quiz_state.current_round = -1;
    pthread_mutex_unlock(&quiz_mutex);
    server_log("[QUIZ] Quiz finished.");
    return NULL;
}

static void handle_quiz_answer(int idx, char answer) {
    int accepted = 0;

    answer = (char)toupper((unsigned char)answer);
    pthread_mutex_lock(&quiz_mutex);
    if (quiz_state.running) {
        pthread_mutex_lock(&clients_mutex);
        if (clients[idx].active && clients[idx].quiz_participant) {
            clients[idx].quiz_answer = answer;
            clients[idx].has_answered = 1;
            send_to_client(clients[idx].sock, "[Quiz] Answer saved. Last answer before time ends counts.\n");
            server_log("[QUIZ] %s answered %c.", clients[idx].nickname, answer);
            accepted = 1;
        }
        pthread_mutex_unlock(&clients_mutex);
    }
    pthread_mutex_unlock(&quiz_mutex);

    if (!accepted) {
        pthread_mutex_lock(&clients_mutex);
        if (clients[idx].active) {
            send_to_client(clients[idx].sock, "[Quiz] You are not a participant in the current quiz.\n");
        }
        pthread_mutex_unlock(&clients_mutex);
    }
}

// AI-assisted implementation: validates starter ownership and snapshots participants.
static void handle_quiz_start(int idx) {
    pthread_t tid;
    int can_start = 0;

    pthread_mutex_lock(&quiz_mutex);
    pthread_mutex_lock(&clients_mutex);
    if (!quiz_state.running && quiz_state.starter_set && clients[idx].active &&
        quiz_state.starter_sock == clients[idx].sock && client_on_quiz_block_locked(idx)) {
        can_start = 1;
        quiz_state.running = 1;
        shuffle_question_indices(quiz_state.question_indices);

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].active && clients[i].room_id == 2) {
                clients[i].quiz_participant = 1;
                clients[i].quiz_score = 0;
                clients[i].quiz_answer = '-';
                clients[i].has_answered = 0;
            } else if (clients[i].active) {
                clients[i].quiz_participant = 0;
            }
        }
        broadcast_quiz_viewers_locked("[Quiz] New quiz started. Participants are users currently in Room 2.\n");
        server_log("[QUIZ] %s started a quiz.", clients[idx].nickname);
    } else if (clients[idx].active) {
        send_to_client(clients[idx].sock, "[Quiz] /start is allowed only for the first Q visitor standing on Q while no quiz is running.\n");
    }
    pthread_mutex_unlock(&clients_mutex);
    pthread_mutex_unlock(&quiz_mutex);

    if (can_start) {
        if (pthread_create(&tid, NULL, quiz_thread, NULL) == 0) {
            pthread_detach(tid);
        }
    }
}

static void handle_input(int idx, char *input) {
    trim_newline(input);
    if (input[0] == '\0') {
        return;
    }

    if (strcmp(input, "w") == 0 || strcmp(input, "\033[A") == 0) {
        handle_move(idx, 0, -1);
    } else if (strcmp(input, "s") == 0 || strcmp(input, "\033[B") == 0) {
        handle_move(idx, 0, 1);
    } else if (strcmp(input, "a") == 0 || strcmp(input, "\033[D") == 0) {
        handle_move(idx, -1, 0);
    } else if (strcmp(input, "d") == 0 || strcmp(input, "\033[C") == 0) {
        handle_move(idx, 1, 0);
    } else if ((input[0] == 'O' || input[0] == 'o' || input[0] == 'X' || input[0] == 'x') && input[1] == '\0') {
        handle_quiz_answer(idx, input[0]);
    } else if (strncmp(input, "/chat ", 6) == 0) {
        handle_chat(idx, input + 6);
    } else if (strncmp(input, "/c ", 3) == 0) {
        handle_chat(idx, input + 3);
    } else if (strncmp(input, "/shout ", 7) == 0) {
        handle_shout(idx, input + 7);
    } else if (strncmp(input, "/s ", 3) == 0) {
        handle_shout(idx, input + 3);
    } else if (strcmp(input, "/start") == 0) {
        handle_quiz_start(idx);
    } else if (strcmp(input, "/map") == 0) {
        pthread_mutex_lock(&clients_mutex);
        if (clients[idx].active) {
            send_map_to_client_locked(idx);
        }
        pthread_mutex_unlock(&clients_mutex);
    } else if (strcmp(input, "/help") == 0) {
        pthread_mutex_lock(&clients_mutex);
        if (clients[idx].active) {
            send_help(clients[idx].sock);
        }
        pthread_mutex_unlock(&clients_mutex);
    } else if (strcmp(input, "/quit") == 0 || strcmp(input, "/exit") == 0) {
        disconnect_client(idx);
    } else {
        pthread_mutex_lock(&clients_mutex);
        if (clients[idx].active) {
            send_to_client(clients[idx].sock, "[Error] Invalid command. Type /help for command list.\n");
        }
        pthread_mutex_unlock(&clients_mutex);
    }
}

// AI-assisted implementation: performs LOGIN protocol and enforces unique nickname/avatar.
static int register_client(int idx) {
    char line[MAX_MSG];
    char nick[MAX_NICK];
    char avatar_text[16];
    int sock = clients[idx].sock;

    send_to_client(sock, "WELCOME TCP Multi-room Avatar Chat System\n");
    send_to_client(sock, "Send LOGIN|nickname|avatar to enter.\n");

    while (1) {
        int n = recv_line(sock, line, sizeof(line));
        if (n <= 0) {
            return 0;
        }

        memset(nick, 0, sizeof(nick));
        memset(avatar_text, 0, sizeof(avatar_text));
        if (sscanf(line, "LOGIN|%31[^|]|%15s", nick, avatar_text) != 2 || strlen(avatar_text) != 1) {
            send_to_client(sock, "LOGIN_FAIL|Use LOGIN|nickname|single_avatar\n");
            continue;
        }

        char avatar = avatar_text[0];
        if (!isalnum((unsigned char)avatar)) {
            send_to_client(sock, "LOGIN_FAIL|Avatar must be one alphanumeric character.\n");
            continue;
        }

        pthread_mutex_lock(&clients_mutex);
        if (!nickname_available_locked(nick)) {
            pthread_mutex_unlock(&clients_mutex);
            send_to_client(sock, "LOGIN_FAIL|Nickname already connected.\n");
            server_log("[ERROR] Duplicate nickname rejected: %s.", nick);
            continue;
        }
        if (!avatar_available_locked(avatar)) {
            pthread_mutex_unlock(&clients_mutex);
            send_to_client(sock, "LOGIN_FAIL|Avatar already connected.\n");
            server_log("[ERROR] Duplicate avatar rejected: %c.", avatar);
            continue;
        }

        int sx = 1;
        int sy = 1;
        if (!find_random_spawn_locked(1, &sx, &sy)) {
            pthread_mutex_unlock(&clients_mutex);
            send_to_client(sock, "LOGIN_FAIL|Server is full or no spawn tile is available.\n");
            continue;
        }

        snprintf(clients[idx].nickname, MAX_NICK, "%s", nick);
        clients[idx].avatar = avatar;
        clients[idx].room_id = 1;
        clients[idx].x = sx;
        clients[idx].y = sy;
        clients[idx].quiz_participant = 0;
        clients[idx].quiz_score = 0;
        clients[idx].quiz_answer = '-';
        clients[idx].has_answered = 0;
        clients[idx].table_id = 0;
        send_to_client(sock, "LOGIN_OK\n");

        char join_msg[MAX_MSG];
        snprintf(join_msg, sizeof(join_msg), "[Lobby] %s joined with avatar %c.\n", nick, avatar);
        broadcast_room_locked(1, join_msg);
        send_help(sock);
        broadcast_maps_to_room_locked(1);
        pthread_mutex_unlock(&clients_mutex);

        server_log("[LOGIN] %s/%c spawned at Lobby (%d,%d).", nick, avatar, sx, sy);
        return 1;
    }
}

static void disconnect_client(int idx) {
    char nick[MAX_NICK];
    int sock = -1;
    int room_id = 0;
    int table_id = 0;
    int was_active = 0;

    pthread_mutex_lock(&clients_mutex);
    if (clients[idx].active) {
        was_active = 1;
        sock = clients[idx].sock;
        room_id = clients[idx].room_id;
        table_id = clients[idx].table_id;
        snprintf(nick, sizeof(nick), "%s", clients[idx].nickname);
        clients[idx].active = 0;
        clients[idx].sock = -1;
        clients[idx].nickname[0] = '\0';
        clients[idx].avatar = '?';
        clients[idx].table_id = 0;
        clients[idx].quiz_participant = 0;

        if (room_id == 1) {
            char msg[MAX_MSG];
            snprintf(msg, sizeof(msg), "[Lobby] %s left the Lobby.\n", nick);
            broadcast_room_locked(1, msg);
        }
        if (table_id > 0) {
            char msg[MAX_MSG];
            snprintf(msg, sizeof(msg), "[Table %d] %s left the table.\n", table_id, nick);
            broadcast_table_locked(table_id, msg);
            if (table_member_count_locked(table_id) == 0) {
                clear_table_log(table_id);
                server_log("[TABLE] Table %d empty. Private chat logs cleared.", table_id);
            }
        }
        if (room_id >= 1 && room_id <= MAX_ROOMS) {
            broadcast_maps_to_room_locked(room_id);
        }
    }
    pthread_mutex_unlock(&clients_mutex);

    pthread_mutex_lock(&quiz_mutex);
    if (was_active && quiz_state.starter_set && quiz_state.starter_sock == sock) {
        quiz_state.starter_set = 0;
    }
    pthread_mutex_unlock(&quiz_mutex);

    if (was_active) {
        close(sock);
        server_log("[DISCONNECT] %s disconnected.", nick);
    }
}

static void *client_thread(void *arg) {
    int idx = *(int *)arg;
    free(arg);
    char line[MAX_MSG];

    if (!register_client(idx)) {
        disconnect_client(idx);
        return NULL;
    }

    while (1) {
        int n;
        pthread_mutex_lock(&clients_mutex);
        int sock = clients[idx].sock;
        int active = clients[idx].active;
        pthread_mutex_unlock(&clients_mutex);

        if (!active) {
            break;
        }

        n = recv_line(sock, line, sizeof(line));
        if (n <= 0) {
            break;
        }
        handle_input(idx, line);
    }

    disconnect_client(idx);
    return NULL;
}

static int allocate_client_slot(int sock) {
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].active) {
            memset(&clients[i], 0, sizeof(clients[i]));
            clients[i].sock = sock;
            clients[i].active = 1;
            clients[i].avatar = '?';
            pthread_mutex_unlock(&clients_mutex);
            return i;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
    return -1;
}

// AI-assisted implementation: Linux TCP server setup with pthread per connected client.
int main(int argc, char *argv[]) {
    int port = DEFAULT_PORT;
    int server_sock;
    struct sockaddr_in server_addr;

    if (argc >= 2) {
        port = atoi(argv[1]);
    }

    srand((unsigned int)time(NULL));
    signal(SIGPIPE, SIG_IGN);
    init_rooms();
    init_quiz_questions();
    init_tables();

    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons((uint16_t)port);

    if (bind(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(server_sock);
        return 1;
    }

    if (listen(server_sock, 16) < 0) {
        perror("listen");
        close(server_sock);
        return 1;
    }

    server_log("[SERVER] Listening on port %d.", port);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_sock = accept(server_sock, (struct sockaddr *)&client_addr, &addr_len);
        if (client_sock < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            continue;
        }

        server_log("[CONNECT] %s:%d connected.",
                   inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

        int idx = allocate_client_slot(client_sock);
        if (idx < 0) {
            send_to_client(client_sock, "Server full. Try again later.\n");
            close(client_sock);
            server_log("[ERROR] Connection rejected: server full.");
            continue;
        }

        pthread_t tid;
        int *arg = malloc(sizeof(int));
        if (!arg) {
            close(client_sock);
            continue;
        }
        *arg = idx;
        if (pthread_create(&tid, NULL, client_thread, arg) != 0) {
            perror("pthread_create");
            free(arg);
            disconnect_client(idx);
            continue;
        }
        pthread_detach(tid);
    }

    close(server_sock);
    return 0;
}
