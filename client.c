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

static volatile int running = 1;

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

// AI-assisted implementation: continuously prints asynchronous server messages.
static void *recv_thread(void *arg) {
    int sock = *(int *)arg;
    char buf[2048];

    while (running) {
        ssize_t n = recv(sock, buf, sizeof(buf) - 1, 0);
        if (n == 0) {
            printf("\n[Client] Server closed the connection.\n");
            running = 0;
            break;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("recv");
            running = 0;
            break;
        }
        buf[n] = '\0';
        printf("%s", buf);
        fflush(stdout);
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

// AI-assisted implementation: Linux TCP client with input sender and receive thread.
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

    printf("[Client] Type /help for commands. WASD is the stable movement input.\n");
    printf("[Client] Arrow keys may work after pressing Enter, depending on terminal mode.\n");

    char line[MAX_MSG];
    while (running && fgets(line, sizeof(line), stdin)) {
        normalize_input(line);
        if (line[0] == '\0') {
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
    return 0;
}
