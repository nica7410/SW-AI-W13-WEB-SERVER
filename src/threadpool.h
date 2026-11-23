// [threadpool.h] 스레드 풀 — 메인 스레드가 accept한 연결 fd를 큐에 넣고, 워커 N개가 꺼내서 처리 (생산자-소비자)
#ifndef THREADPOOL_H
#define THREADPOOL_H

typedef void (*task_fn)(int connfd);   // 워커가 연결 하나를 처리할 함수
typedef struct threadpool threadpool;  // 내부 구조는 threadpool.c에만 공개

// 워커 nthreads개, 큐 크기 queue_size인 풀 생성. 실패 NULL
threadpool *threadpool_create(int nthreads, int queue_size, task_fn fn);

// 연결 fd를 큐에 넣기. 성공 0, 큐가 가득 차면 -1 (호출한 쪽이 503 응답 후 close)
int threadpool_submit(threadpool *pool, int connfd);

// 쉬고 있는 워커가 없어서 큐에서 기다리는 연결이 있으면 1 (keep-alive 연결을 양보할지 판단)
int threadpool_has_waiting(threadpool *pool);
// 작업을 막 끝내고 큐로 돌아오는 중인 워커는 아직 쉬는 워커로 세지 않으므로, 곧 처리될 작업을
// 대기로 볼 수 있다. 이때는 keep-alive 연결을 조금 일찍 양보할 뿐 응답에는 영향이 없다.

// 큐를 기다리며 쉬고 있는 워커 수 (테스트·모니터링용)
int threadpool_idle_workers(threadpool *pool);

// 종료: 새 작업을 막고, 워커가 큐의 남은 작업을 마치면 join 후 메모리 해제
void threadpool_destroy(threadpool *pool);

#endif
