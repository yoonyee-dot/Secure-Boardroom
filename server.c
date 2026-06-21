#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <ctype.h>

#define DEFAULT_PORT 8080
#define MAX_CLIENTS 20
#define BUFFER_SIZE 1024
#define LOG_FILE "boardroom_log.txt"
#define NOTICE_FILE "server_notice.txt"
#define CONFIG_FILE "server.conf"
#define BACKUP_LIMIT 5000
#define XOR_KEY 0xAA
#define BACKUP_DIR "./secure_backup"

typedef struct {
    int socket;
    char nickname[32];
    int role;
    int is_muted;
    int is_afk;
    char ip_addr[INET_ADDRSTRLEN];
    time_t join_time;
} ClientInfo;

ClientInfo clients[MAX_CLIENTS];
int server_port = DEFAULT_PORT;
int client_count = 0;
int total_messages_processed = 0;
time_t server_start_time;
char global_notice[BUFFER_SIZE] = "등록된 공지사항이 없습니다.";

pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t stats_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t vote_mutex = PTHREAD_MUTEX_INITIALIZER;

int is_voting_active = 0;
int agree_votes = 0;
int disagree_votes = 0;
char current_agenda[BUFFER_SIZE];
char agenda_history[100][BUFFER_SIZE];
int agenda_history_count = 0;

void trim_newline(char *str) {
    int len = strlen(str);
    while (len > 0 && (str[len - 1] == '\n' || str[len - 1] == '\r')) {
        str[--len] = '\0';
    }
}

void load_server_config() {
    FILE *fp = fopen(CONFIG_FILE, "r");
    if (fp != NULL) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            if (strncmp(line, "PORT=", 5) == 0) {
                server_port = atoi(line + 5);
                if (server_port <= 0 || server_port > 65535) {
                    server_port = DEFAULT_PORT;
                }
            }
        }
        fclose(fp);
    }

    fp = fopen(NOTICE_FILE, "r");
    if (fp != NULL) {
        fgets(global_notice, sizeof(global_notice), fp);
        trim_newline(global_notice);
        fclose(fp);
    }
}

void save_notice(const char *notice) {
    FILE *fp = fopen(NOTICE_FILE, "w");
    if (fp != NULL) {
        fprintf(fp, "%s\n", notice);
        fclose(fp);
    }
}

void filter_profanity(char *message) {
    const char *bad_words[] = {"바보", "멍청이", "해킹", "비속어"};
    int num_bad_words = sizeof(bad_words) / sizeof(bad_words[0]);

    for (int i = 0; i < num_bad_words; i++) {
        char *pos = strstr(message, bad_words[i]);
        while (pos != NULL) {
            int len = strlen(bad_words[i]);
            for (int j = 0; j < len; j++) {
                pos[j] = '*';
            }
            pos = strstr(pos + len, bad_words[i]);
        }
    }
}

void encrypt_and_log(const char *message) {
    pthread_mutex_lock(&log_mutex);
    int fd = open(LOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd != -1) {
        char encrypted[2048];
        int len = strlen(message);
        if (len > 2000) {
            len = 2000;
        }

        for (int i = 0; i < len; i++) {
            encrypted[i] = message[i] ^ XOR_KEY;
        }
        encrypted[len] = '\n';

        write(fd, encrypted, len + 1);
        close(fd);
    }
    pthread_mutex_unlock(&log_mutex);
}

void check_and_backup(int force) {
    struct stat st;
    int trigger = force;

    pthread_mutex_lock(&log_mutex);
    if (!trigger && stat(LOG_FILE, &st) == 0 && st.st_size >= BACKUP_LIMIT) {
        trigger = 1;
    }

    if (trigger) {
        mkdir(BACKUP_DIR, 0700);
        pid_t pid = fork();

        if (pid == 0) {
            execlp("tar", "tar", "-czf", "boardroom_backup.tar.gz", LOG_FILE, NULL);
            exit(1);
        } else if (pid > 0) {
            waitpid(pid, NULL, 0);
            char new_path[256];
            sprintf(new_path, "%s/backup_%ld.tar.gz", BACKUP_DIR, time(NULL));
            rename("boardroom_backup.tar.gz", new_path);
            remove(LOG_FILE);
        }
    }
    pthread_mutex_unlock(&log_mutex);
}

void send_to_all_ansi(const char *message, int sender_sock) {
    pthread_mutex_lock(&clients_mutex);
    char ansi_msg[2048];
    snprintf(ansi_msg, sizeof(ansi_msg), "\r\033[2K%s\n> ", message);

    for (int i = 0; i < client_count; i++) {
        if (clients[i].socket != sender_sock) {
            write(clients[i].socket, ansi_msg, strlen(ansi_msg));
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

void send_private_msg(const char *sender_nick, const char *target_nick, const char *msg, int sender_sock) {
    pthread_mutex_lock(&clients_mutex);
    int found = 0;
    char pm_buf[2048];

    snprintf(pm_buf, sizeof(pm_buf), "\r\033[2K[귓속말] %s: %s\n> ", sender_nick, msg);

    for (int i = 0; i < client_count; i++) {
        if (strcmp(clients[i].nickname, target_nick) == 0) {
            write(clients[i].socket, pm_buf, strlen(pm_buf));
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);

    if (!found) {
        char err[] = "\r\033[2K[시스템] 해당 닉네임의 참가자가 없습니다.\n> ";
        write(sender_sock, err, strlen(err));
    }
}

int find_client_index_by_nick(const char *nick) {
    for (int i = 0; i < client_count; i++) {
        if (strcmp(clients[i].nickname, nick) == 0) {
            return i;
        }
    }
    return -1;
}

void execute_admin_command(int sock, int role, char *cmd, char *target, char *extra) {
    if (role < 1) {
        char deny[] = "\r\033[2K[시스템] 권한이 부족합니다.\n> ";
        write(sock, deny, strlen(deny));
        return;
    }

    pthread_mutex_lock(&clients_mutex);
    int idx = find_client_index_by_nick(target);

    if (idx == -1 && strcmp(cmd, "/공지") != 0) {
        char err[] = "\r\033[2K[시스템] 대상을 찾을 수 없습니다.\n> ";
        write(sock, err, strlen(err));
        pthread_mutex_unlock(&clients_mutex);
        return;
    }

    char broadcast_msg[2048];

    if (strcmp(cmd, "/강퇴") == 0 && role == 2) {
        snprintf(broadcast_msg, sizeof(broadcast_msg), "[시스템] %s님이 강제 퇴장되었습니다.", clients[idx].nickname);
        char kick_msg[] = "\r\033[2K[시스템] 관리자에 의해 강퇴되었습니다.\n";
        write(clients[idx].socket, kick_msg, strlen(kick_msg));
        close(clients[idx].socket);

        for (int j = idx; j < client_count - 1; j++) {
            clients[j] = clients[j + 1];
        }
        client_count--;
        pthread_mutex_unlock(&clients_mutex);
        send_to_all_ansi(broadcast_msg, -1);
        return;

    } else if (strcmp(cmd, "/채팅금지") == 0) {
        clients[idx].is_muted = 1;
        snprintf(broadcast_msg, sizeof(broadcast_msg), "[시스템] %s님의 채팅이 금지되었습니다.", clients[idx].nickname);

    } else if (strcmp(cmd, "/금지해제") == 0) {
        clients[idx].is_muted = 0;
        snprintf(broadcast_msg, sizeof(broadcast_msg), "[시스템] %s님의 채팅 금지가 해제되었습니다.", clients[idx].nickname);

    } else if (strcmp(cmd, "/부의장임명") == 0 && role == 2) {
        clients[idx].role = 1;
        snprintf(broadcast_msg, sizeof(broadcast_msg), "[시스템] %s님이 부의장으로 임명되었습니다.", clients[idx].nickname);

    } else if (strcmp(cmd, "/공지") == 0 && role == 2) {
        if (target != NULL) {
            strncpy(global_notice, target, sizeof(global_notice) - 1);
            if(extra != NULL) {
                strcat(global_notice, " ");
                strcat(global_notice, extra);
            }
            save_notice(global_notice);
            snprintf(broadcast_msg, sizeof(broadcast_msg), "[새 공지사항] %s", global_notice);
        }
    } else {
        pthread_mutex_unlock(&clients_mutex);
        return;
    }

    pthread_mutex_unlock(&clients_mutex);
    send_to_all_ansi(broadcast_msg, -1);
}

void show_server_stats(int sock) {
    char buf[2048];
    long uptime = (long)(time(NULL) - server_start_time);

    pthread_mutex_lock(&stats_mutex);
    int msgs = total_messages_processed;
    pthread_mutex_unlock(&stats_mutex);

    snprintf(buf, sizeof(buf), "\r\033[2K=== [서버 상태] ===\n업타임: %ld초\n접속자: %d/%d\n처리 메시지: %d\n==================\n> ",
             uptime, client_count, MAX_CLIENTS, msgs);
    write(sock, buf, strlen(buf));
}

void *vote_timer_thread(void *arg) {
    sleep(30);
    pthread_mutex_lock(&vote_mutex);

    if (is_voting_active) {
        is_voting_active = 0;
        char res[2048];
        snprintf(res, sizeof(res), "==== 투표 종료 ====\n안건: %s\n결과: 찬성 %d | 반대 %d\n==================",
                 current_agenda, agree_votes, disagree_votes);
        send_to_all_ansi(res, -1);
        encrypt_and_log(res);
    }

    pthread_mutex_unlock(&vote_mutex);
    pthread_exit(NULL);
}

void *handle_client(void *arg) {
    int client_sock = *(int*)arg;
    free(arg);

    char buffer[BUFFER_SIZE];
    char send_buf[2048];
    int read_size;
    char my_nick[32] = {0};
    int my_idx = -1;

    char msg_prompt1[] = "사용할 닉네임을 입력하세요: ";
    write(client_sock, msg_prompt1, strlen(msg_prompt1));

    while ((read_size = read(client_sock, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[read_size] = '\0';
        trim_newline(buffer);

        if (strlen(buffer) < 2) {
            char msg_prompt2[] = "2글자 이상 입력하세요: ";
            write(client_sock, msg_prompt2, strlen(msg_prompt2));
            continue;
        }

        pthread_mutex_lock(&clients_mutex);
        int dup = 0;
        for (int i = 0; i < client_count; i++) {
            if (strcmp(clients[i].nickname, buffer) == 0) {
                dup = 1;
                break;
            }
        }

        if (dup) {
            pthread_mutex_unlock(&clients_mutex);
            char msg_prompt3[] = "이미 사용중인 닉네임입니다. 다시 입력: ";
            write(client_sock, msg_prompt3, strlen(msg_prompt3));
            continue;
        }

        strcpy(my_nick, buffer);
        clients[client_count].socket = client_sock;
        strcpy(clients[client_count].nickname, my_nick);
        clients[client_count].role = (client_count == 0) ? 2 : 0;
        clients[client_count].is_muted = 0;
        clients[client_count].is_afk = 0;
        my_idx = client_count;
        client_count++;
        pthread_mutex_unlock(&clients_mutex);
        break;
    }

    if (read_size <= 0) {
        close(client_sock);
        pthread_exit(NULL);
    }

    snprintf(send_buf, sizeof(send_buf), "\r\033[2K[입장] %s님이 들어왔습니다. %s",
             my_nick, (clients[my_idx].role == 2) ? "(의장 권한 부여됨)" : "");
    send_to_all_ansi(send_buf, client_sock);

    snprintf(send_buf, sizeof(send_buf), "\r\033[2K[현재 공지] %s\n> ", global_notice);
    write(client_sock, send_buf, strlen(send_buf));

    while ((read_size = read(client_sock, buffer, sizeof(buffer) - 1)) > 0) {
        buffer[read_size] = '\0';
        trim_newline(buffer);

        if (strlen(buffer) == 0) {
            write(client_sock, "> ", 2);
            continue;
        }

        pthread_mutex_lock(&stats_mutex);
        total_messages_processed++;
        pthread_mutex_unlock(&stats_mutex);

        pthread_mutex_lock(&clients_mutex);
        int muted = clients[my_idx].is_muted;
        int role = clients[my_idx].role;
        clients[my_idx].is_afk = 0;
        pthread_mutex_unlock(&clients_mutex);

        if (muted && buffer[0] != '/') {
            char mute_msg[] = "\r\033[2K[시스템] 채팅이 금지된 상태입니다.\n> ";
            write(client_sock, mute_msg, strlen(mute_msg));
            continue;
        }

        filter_profanity(buffer);

        if (buffer[0] == '/') {
            char *cmd = strtok(buffer, " ");
            char *arg1 = strtok(NULL, " ");
            char *arg2 = strtok(NULL, "");

            if (strcmp(cmd, "/귓속말") == 0 && arg1 && arg2) {
                send_private_msg(my_nick, arg1, arg2, client_sock);
            } else if (strcmp(cmd, "/상태") == 0) {
                show_server_stats(client_sock);
            } else if (strcmp(cmd, "/자리비움") == 0) {
                pthread_mutex_lock(&clients_mutex);
                clients[my_idx].is_afk = 1;
                pthread_mutex_unlock(&clients_mutex);
                snprintf(send_buf, sizeof(send_buf), "[시스템] %s님이 자리를 비웠습니다.", my_nick);
                send_to_all_ansi(send_buf, -1);
            } else if (strcmp(cmd, "/발의") == 0 && arg1) {
                if (role < 2) {
                    char err_role[] = "\r\033[2K[시스템] 의장 전용.\n> ";
                    write(client_sock, err_role, strlen(err_role));
                    continue;
                }

                pthread_mutex_lock(&vote_mutex);
                if (is_voting_active) {
                    char err_vote[] = "\r\033[2K진행중인 투표가 있습니다.\n> ";
                    write(client_sock, err_vote, strlen(err_vote));
                    pthread_mutex_unlock(&vote_mutex);
                    continue;
                }

                is_voting_active = 1;
                agree_votes = 0;
                disagree_votes = 0;
                snprintf(current_agenda, sizeof(current_agenda), "%s %s", arg1, arg2 ? arg2 : "");

                if (agenda_history_count < 100) {
                    strcpy(agenda_history[agenda_history_count++], current_agenda);
                }

                snprintf(send_buf, sizeof(send_buf), "[의장 발의] %s\n(30초 투표: /투표 찬성 또는 /투표 반대)", current_agenda);
                send_to_all_ansi(send_buf, -1);

                pthread_t tid;
                pthread_create(&tid, NULL, vote_timer_thread, NULL);
                pthread_detach(tid);
                pthread_mutex_unlock(&vote_mutex);

            } else if (strcmp(cmd, "/투표") == 0 && arg1) {
                pthread_mutex_lock(&vote_mutex);
                if (!is_voting_active) {
                    char err_novote[] = "\r\033[2K진행중인 투표가 없습니다.\n> ";
                    write(client_sock, err_novote, strlen(err_novote));
                } else {
                    if (strcmp(arg1, "찬성") == 0) {
                        agree_votes++;
                    } else if (strcmp(arg1, "반대") == 0) {
                        disagree_votes++;
                    }
                    snprintf(send_buf, sizeof(send_buf), "[투표] %s님 참여 (찬성:%d/반대:%d)",
                             my_nick, agree_votes, disagree_votes);
                    send_to_all_ansi(send_buf, -1);
                }
                pthread_mutex_unlock(&vote_mutex);

            } else if (role > 0) {
                execute_admin_command(client_sock, role, cmd, arg1, arg2);
            } else {
                char err_cmd[] = "\r\033[2K[시스템] 알 수 없거나 권한이 없는 명령어입니다.\n> ";
                write(client_sock, err_cmd, strlen(err_cmd));
            }
        } else {
            snprintf(send_buf, sizeof(send_buf), "%s: %s", my_nick, buffer);
            send_to_all_ansi(send_buf, client_sock);
            encrypt_and_log(send_buf);
        }

        write(client_sock, "> ", 2);
        check_and_backup(0);
    }

    pthread_mutex_lock(&clients_mutex);
    int found_idx = -1;

    for (int i = 0; i < client_count; i++) {
        if (clients[i].socket == client_sock) {
            found_idx = i;
            break;
        }
    }

    if (found_idx != -1) {
        snprintf(send_buf, sizeof(send_buf), "[퇴장] %s님이 나갔습니다.", clients[found_idx].nickname);
        for (int j = found_idx; j < client_count - 1; j++) {
            clients[j] = clients[j + 1];
        }
        client_count--;

        if (found_idx == 0 && client_count > 0) {
            clients[0].role = 2;
            char auth_msg[256];
            snprintf(auth_msg, sizeof(auth_msg), "[시스템] 의장 퇴장으로 %s님이 새 의장이 되었습니다.", clients[0].nickname);
            send_to_all_ansi(auth_msg, -1);
        }
    }

    int remaining = client_count;
    pthread_mutex_unlock(&clients_mutex);

    if (found_idx != -1) {
        send_to_all_ansi(send_buf, -1);
    }

    close(client_sock);
    if (remaining == 0) {
        check_and_backup(1);
    }

    pthread_exit(NULL);
}

int main() {
    server_start_time = time(NULL);
    load_server_config();

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(server_port);

    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        exit(1);
    }

    if (listen(server_fd, 5) == -1) {
        exit(1);
    }

    printf("서버 포트 %d에서 대기 중...\n", server_port);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int *new_sock = malloc(sizeof(int));

        *new_sock = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (*new_sock < 0) {
            free(new_sock);
            continue;
        }

        pthread_mutex_lock(&clients_mutex);
        if (client_count < MAX_CLIENTS) {
            pthread_t tid;
            pthread_create(&tid, NULL, handle_client, new_sock);
            pthread_detach(tid);
        } else {
            char full_msg[] = "서버가 가득 찼습니다.\n";
            write(*new_sock, full_msg, strlen(full_msg));
            close(*new_sock);
            free(new_sock);
        }
        pthread_mutex_unlock(&clients_mutex);
    }

    close(server_fd);
    return 0;
}
