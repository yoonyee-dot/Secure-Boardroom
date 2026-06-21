// 회의 참가자(연결 수신측) 터미널 클라이언트 프로그램

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>
#include <netdb.h>

#define SERVER_HOST "0.tcp.jp.ngrok.io"
#define PORT 28096
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
        printf("\n[알림] 서버와의 연결이 안전하게 종료되었습니다.\n");
    } else {
        perror("\n[오류] 메시지 수신 실패");
    }
    exit(0);
}

int main() {
    int sock;
    struct sockaddr_in serv_addr;
    struct hostent *server;
    pthread_t recv_thread;
    char message[BUFFER_SIZE];

    server = gethostbyname(SERVER_HOST);
    if (server == NULL) {
        fprintf(stderr, "[오류] ngrok 주소를 찾을 수 없습니다.\n");
        return -1;
    }

    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("소켓 생성 오류");
        return -1;
    }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);
    memcpy(&serv_addr.sin_addr.s_addr, server->h_addr_list[0], server->h_length);

    printf("[연결 중] %s:%d 서버에 접속을 시도합니다...\n", SERVER_HOST, PORT);
    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("서버 연결 실패 (서버가 켜져 있는지 확인하세요)");
        return -1;
    }

    printf("=== Secure Boardroom 터미널 접속 성공 ===\n");
    printf("[명령어 안내]\n");
    printf("- 일반 채팅: 내용을 입력하고 Enter\n");
    printf("- 의장 안건발의: '/발의 [안건내용]' (참가자 1만 가능)\n");
    printf("- 안건 투표: '/찬성' 또는 '/반대'\n");
    printf("- 연결 종료: 'quit' 입력\n");
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
