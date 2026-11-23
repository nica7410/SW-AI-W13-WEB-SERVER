#include "test.h"
#include "threadpool.h"
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

// 워커 하나가 작업을 잡고 있는 동안 큐에 들어온 작업만 "대기"로 센다.
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int started, released;

static void task(int id) {
    pthread_mutex_lock(&lock);
    started = id;
    pthread_cond_broadcast(&changed);
    while (released < id) pthread_cond_wait(&changed, &lock);
    pthread_mutex_unlock(&lock);
}

static void wait_started(int id) {
    pthread_mutex_lock(&lock);
    while (started != id) pthread_cond_wait(&changed, &lock);
    pthread_mutex_unlock(&lock);
}

static void release(int id) {
    pthread_mutex_lock(&lock);
    released = id;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
}

static void pause_ms(int ms) {
    struct timespec t = {.tv_nsec = ms * 1000000L};
    nanosleep(&t, NULL);
}

int main(void) {
    alarm(5);
    threadpool *pool = threadpool_create(1, 4, task);
    CHECK(pool != NULL);
    if (!pool) return TEST_RESULT();
    while (threadpool_idle_workers(pool) != 1) pause_ms(1);   // 워커가 큐를 기다리며 잠들 때까지
    CHECK(threadpool_has_waiting(pool) == 0);

    // 쉬는 워커가 가져갈 작업은 깨어나기 전이라도 대기로 세지 않는다.
    CHECK(threadpool_submit(pool, 1) == 0);
    CHECK(threadpool_has_waiting(pool) == 0);
    wait_started(1);

    // 유일한 워커가 바쁘면 새 작업은 기다린다.
    CHECK(threadpool_submit(pool, 2) == 0);
    CHECK(threadpool_has_waiting(pool) == 1);

    release(1);
    wait_started(2);
    CHECK(threadpool_has_waiting(pool) == 0);
    release(2);
    threadpool_destroy(pool);
    return TEST_RESULT();
}
