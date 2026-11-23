// [server.h] 서버 본체 — 듣기 소켓 열기, accept 루프, 연결 하나 처리(요청 분기)
#ifndef SERVER_H
#define SERVER_H

#define KEEPALIVE_TIMEOUT_SEC 5   // keep-alive 연결이 이 시간 동안 다음 요청을 안 보내면 닫는다 (워커 점유 방지)

typedef struct {
    const char *port;          // -p  듣기 포트 (기본 "8080")
    const char *root;          // -r  정적 파일 루트 (기본 "./www")
    const char *cgi_dir;       // -g  CGI 프로그램 폴더 (기본 "./cgi-bin")
    const char *routes_path;   // -c  Reverse Proxy 설정 파일. NULL이면 Reverse Proxy 사용 안 함
    int forward_enabled;       // -f  Forward Proxy 허용 (기본 0 — 아무나 쓰는 열린 프록시가 되지 않도록 명시적으로 켬)
    int threads;               // -t  워커 스레드 수 (기본 8)
    int queue_size;            // -q  연결 대기 큐 크기 (기본 1024). 큐는 fd만 담으므로 크게 잡아도 부담이 작다
    int root_explicit;         // -r가 주어지면 루트 열기 실패 시 기동 중단
    int verbose;               // -v 요청별 접근 로그를 stderr에 기록
} server_config;

// 서버 실행: 설정 로드 → open_listenfd → 스레드 풀 생성 → 종료 요청이 올 때까지 accept → submit
// 정상 종료 0, 시작 실패 -1
int run_server(const server_config *cfg);

// 연결 하나 처리 (워커 스레드에서 호출)
void handle_connection(int connfd);

// 종료 요청 (SIGINT/SIGTERM 핸들러에서 호출 — 플래그만 세운다)
void request_stop(void);

#endif
