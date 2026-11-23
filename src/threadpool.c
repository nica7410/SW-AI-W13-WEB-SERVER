#include "threadpool.h"
#include <pthread.h>
#include <stdlib.h>

struct threadpool {
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    int *queue;
    int capacity, head, tail, count;
    pthread_t *threads;
    int nthreads, stopping;
    int idle;   // 큐를 기다리며 잠든 워커 수. 깨어나 작업을 꺼낼 때 count와 함께 줄어든다
    task_fn fn;
};

static void *worker(void *arg) {
    threadpool *pool = arg;
    for (;;) {
        pthread_mutex_lock(&pool->lock);
        pool->idle++;
        while (pool->count == 0 && !pool->stopping)
            pthread_cond_wait(&pool->not_empty, &pool->lock);
        pool->idle--;
        if (pool->count == 0) {
            pthread_mutex_unlock(&pool->lock);
            return NULL;
        }
        int fd = pool->queue[pool->head];
        pool->head = (pool->head + 1) % pool->capacity;
        pool->count--;
        pthread_mutex_unlock(&pool->lock);
        pool->fn(fd);
    }
}

threadpool *threadpool_create(int nthreads, int queue_size, task_fn fn) {
    if (!fn || nthreads < 1 || queue_size < 1) return NULL;
    threadpool *pool = calloc(1, sizeof(*pool));
    if (!pool) return NULL;
    pool->capacity = queue_size;
    pool->fn = fn;
    pool->queue = calloc((size_t)queue_size, sizeof(*pool->queue));
    pool->threads = calloc((size_t)nthreads, sizeof(*pool->threads));
    if (!pool->queue || !pool->threads) goto fail;
    if (pthread_mutex_init(&pool->lock, NULL) != 0) goto fail;
    if (pthread_cond_init(&pool->not_empty, NULL) != 0) {
        pthread_mutex_destroy(&pool->lock);
        goto fail;
    }
    while (pool->nthreads < nthreads) {
        if (pthread_create(&pool->threads[pool->nthreads], NULL, worker, pool) != 0) {
            threadpool_destroy(pool);
            return NULL;
        }
        pool->nthreads++;
    }
    return pool;
fail:
    free(pool->threads);
    free(pool->queue);
    free(pool);
    return NULL;
}

int threadpool_submit(threadpool *pool, int connfd) {
    pthread_mutex_lock(&pool->lock);
    if (pool->stopping || pool->count == pool->capacity) {
        pthread_mutex_unlock(&pool->lock);
        return -1;
    }
    pool->queue[pool->tail] = connfd;
    pool->tail = (pool->tail + 1) % pool->capacity;
    pool->count++;
    pthread_cond_signal(&pool->not_empty);
    pthread_mutex_unlock(&pool->lock);
    return 0;
}

int threadpool_has_waiting(threadpool *pool) {
    // 제출 직후 아직 깨어나지 않은 워커는 idle에 남아 있으므로 그 작업은 대기로 세지 않는다.
    pthread_mutex_lock(&pool->lock);
    int waiting = pool->count > pool->idle;
    pthread_mutex_unlock(&pool->lock);
    return waiting;
}

int threadpool_idle_workers(threadpool *pool) {
    pthread_mutex_lock(&pool->lock);
    int idle = pool->idle;
    pthread_mutex_unlock(&pool->lock);
    return idle;
}

void threadpool_destroy(threadpool *pool) {
    pthread_mutex_lock(&pool->lock);
    pool->stopping = 1;
    pthread_cond_broadcast(&pool->not_empty);
    pthread_mutex_unlock(&pool->lock);
    for (int i = 0; i < pool->nthreads; i++)
        pthread_join(pool->threads[i], NULL);
    pthread_cond_destroy(&pool->not_empty);
    pthread_mutex_destroy(&pool->lock);
    free(pool->threads);
    free(pool->queue);
    free(pool);
}
