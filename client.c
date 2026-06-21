#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>

#define PORT 8080
#define BUFFER_SIZE 1024

void *recv_msg(void *socket_desc) {
    int sock = *(int *)socket_desc;
    char buffer[BUFFER_SIZE];
    int read_size;

    while ((read_size = read(sock, buffer, BUFFER_SIZE - 1)) > 0) {
        buffer[read_size] = '\0';
        printf("%s", buffer);
        fflush(stdout);
    }

    if (read_size == 0) {
        printf("\n[알림] 서버와 연결이 끊어졌습니다.\n");
    } else if (read_size == -1) {
        perror("\n[오류] 메시지 수신 실패");
    }

    exit(0);
}

int main() {
    int sock;
    struct sockaddr_in serv_addr;
    pthread_t recv_thread;
    char message[BUFFER_SIZE];

    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("소켓 생성 오류");
        return -1;
    }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);

    if (inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr) <= 0) {
        perror("유효하지 않은 주소");
        return -1;
    }

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("연결 실패");
        return -1;
    }

    printf("=== Secure Boardroom 터미널에 접속했습니다 ===\n");
    printf("사용법: 메시지를 입력하고 Enter를 누르세요.\n");
    printf("투표 명령어: '/투표 찬성' 또는 '/투표 반대'\n");
    printf("종료: 'quit' 입력\n");
    printf("==============================================\n");

    if (pthread_create(&recv_thread, NULL, recv_msg, (void *)&sock) < 0) {
        perror("수신 스레드 생성 실패");
        return -1;
    }
    
    while (1) {
        fgets(message, BUFFER_SIZE, stdin);

        if (strncmp(message, "quit", 4) == 0) {
            break;
        }
        write(sock, message, strlen(message));
    }

    close(sock);
    return 0;
}
