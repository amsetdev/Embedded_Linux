/**
 * @file msg_queue.c
 * @brief Thread-safe bounded message queue (ring buffer).
 *
 * Uses a circular array of dynamically allocated strings with
 * mutex + condition variable synchronisation. Drop-oldest policy
 * ensures the producer (Modbus polling) never blocks.
 */

#include "msg_queue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* -------------------------------------------------------------------------- */
/* Internal structure                                                         */
/* -------------------------------------------------------------------------- */

/** @brief Message queue internal state. */
struct msg_queue
{
    char           **slots;     /**< Circular array of payload strings. */
    int              capacity;  /**< Maximum number of slots.           */
    int              head;      /**< Next write position.               */
    int              tail;      /**< Next read position.                */
    int              count;     /**< Current number of messages.        */
    int              shutdown;  /**< 1 = queue is shutting down.        */
    pthread_mutex_t  mutex;     /**< Protects all fields.               */
    pthread_cond_t   cond;      /**< Signals new message or shutdown.   */
};

/* -------------------------------------------------------------------------- */
/* API                                                                        */
/* -------------------------------------------------------------------------- */

/**
 * @brief Creates a new message queue.
 *
 * @param capacity  Maximum messages. 0 = default.
 * @return Queue handle or NULL.
 */
msg_queue_t *msg_queue_create(int capacity)
{
    msg_queue_t *q = calloc(1, sizeof(*q));

    if (!q)
    {
        perror("[MSGQ] calloc queue");
        return NULL;
    }

    q->capacity = (capacity > 0) ? capacity : MSG_QUEUE_CAPACITY;

    q->slots = calloc((size_t)q->capacity, sizeof(char *));

    if (!q->slots)
    {
        perror("[MSGQ] calloc slots");
        free(q);
        return NULL;
    }

    q->head     = 0;
    q->tail     = 0;
    q->count    = 0;
    q->shutdown = 0;

    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);

    printf("[MSGQ] Created (capacity=%d)\n", q->capacity);

    return q;
}

/**
 * @brief Pushes a message onto the queue (never blocks).
 *
 * @param q        Queue handle.
 * @param payload  Null-terminated string to copy.
 * @return 0 on success, -1 on error.
 */
int msg_queue_push(msg_queue_t *q, const char *payload)
{
    if (!q || !payload)
        return -1;

    char *copy = strdup(payload);

    if (!copy)
    {
        perror("[MSGQ] strdup");
        return -1;
    }

    pthread_mutex_lock(&q->mutex);

    if (q->count == q->capacity)
    {
        /*
         * Queue full — drop the oldest message at tail.
         */
        fprintf(stderr,
                "[MSGQ] Queue full (%d/%d) — dropping oldest message\n",
                q->count, q->capacity);

        free(q->slots[q->tail]);
        q->slots[q->tail] = NULL;

        q->tail = (q->tail + 1) % q->capacity;
        q->count--;
    }

    q->slots[q->head] = copy;
    q->head = (q->head + 1) % q->capacity;
    q->count++;

    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);

    return 0;
}

/**
 * @brief Pops a message from the queue (blocks until available).
 *
 * @param q  Queue handle.
 * @return Payload string (caller must free), or NULL on shutdown.
 */
char *msg_queue_pop(msg_queue_t *q)
{
    if (!q)
        return NULL;

    pthread_mutex_lock(&q->mutex);

    while (q->count == 0 && !q->shutdown)
    {
        pthread_cond_wait(&q->cond, &q->mutex);
    }

    if (q->count == 0)
    {
        /* Shutdown with empty queue. */
        pthread_mutex_unlock(&q->mutex);
        return NULL;
    }

    char *msg = q->slots[q->tail];
    q->slots[q->tail] = NULL;

    q->tail = (q->tail + 1) % q->capacity;
    q->count--;

    pthread_mutex_unlock(&q->mutex);

    return msg;
}

/**
 * @brief Signals the queue to shut down.
 *
 * @param q  Queue handle.
 */
void msg_queue_shutdown(msg_queue_t *q)
{
    if (!q)
        return;

    pthread_mutex_lock(&q->mutex);

    q->shutdown = 1;

    pthread_cond_broadcast(&q->cond);
    pthread_mutex_unlock(&q->mutex);

    printf("[MSGQ] Shutdown signalled\n");
}

/**
 * @brief Destroys the queue and frees all resources.
 *
 * @param q  Queue handle.
 */
void msg_queue_destroy(msg_queue_t *q)
{
    if (!q)
        return;

    /* Free any remaining messages. */
    for (int i = 0; i < q->capacity; i++)
    {
        free(q->slots[i]);
        q->slots[i] = NULL;
    }

    free(q->slots);

    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->cond);

    free(q);

    printf("[MSGQ] Destroyed\n");
}
