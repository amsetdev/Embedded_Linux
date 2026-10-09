/**
 * @file msg_queue.h
 * @brief Thread-safe message queue for decoupling producers and consumers.
 *
 * Implements a bounded ring buffer of dynamically allocated strings.
 * When the queue is full, the oldest message is dropped (drop-oldest
 * policy) so the producer never blocks.
 *
 * Typical usage:
 * - Producer (Modbus RTU thread): calls msg_queue_push() after
 *   building the MQTT payload.
 * - Consumer (MQTT publisher thread): calls msg_queue_pop() which
 *   blocks until a message is available or shutdown is signalled.
 */

#ifndef MSG_QUEUE_H
#define MSG_QUEUE_H

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Constants                                                                   */
/* -------------------------------------------------------------------------- */

/** @brief Default queue capacity (number of messages). */
#define MSG_QUEUE_CAPACITY  32

/* -------------------------------------------------------------------------- */
/* Types                                                                      */
/* -------------------------------------------------------------------------- */

/** @brief Opaque message queue handle. */
typedef struct msg_queue msg_queue_t;

/* -------------------------------------------------------------------------- */
/* API                                                                        */
/* -------------------------------------------------------------------------- */

/**
 * @brief Creates a new message queue.
 *
 * @param capacity  Maximum number of messages. Use 0 for the default
 *                  (MSG_QUEUE_CAPACITY).
 *
 * @return Pointer to the queue, or NULL on failure.
 */
msg_queue_t *msg_queue_create(int capacity);

/**
 * @brief Pushes a message onto the queue.
 *
 * The payload string is copied internally. If the queue is full,
 * the oldest message is dropped and a warning is logged.
 * This function never blocks the caller.
 *
 * @param q        Queue handle.
 * @param payload  Null-terminated payload string.
 *
 * @return 0 on success, -1 on error.
 */
int msg_queue_push(msg_queue_t *q, const char *payload);

/**
 * @brief Pops a message from the queue (blocking).
 *
 * Blocks until a message is available or the queue is shut down.
 * The caller must free the returned string with free().
 *
 * @param q  Queue handle.
 *
 * @return Dynamically allocated payload string, or NULL on shutdown.
 */
char *msg_queue_pop(msg_queue_t *q);

/**
 * @brief Signals the queue to shut down.
 *
 * Wakes any thread blocked in msg_queue_pop() so it can exit.
 *
 * @param q  Queue handle.
 */
void msg_queue_shutdown(msg_queue_t *q);

/**
 * @brief Destroys the queue and frees all resources.
 *
 * Any remaining messages are freed. The queue must not be used
 * after this call.
 *
 * @param q  Queue handle.
 */
void msg_queue_destroy(msg_queue_t *q);

#ifdef __cplusplus
}
#endif

#endif /* MSG_QUEUE_H */
