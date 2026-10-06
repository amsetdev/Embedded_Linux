/**
 * @file store_forward.h
 * @brief Store-and-forward decisions for telemetry that could not be published
 *        (no file system or MQTT dependency, host-tested).
 *
 * A payload that can't be published is stored as one file named
 * "<unix ms>.txt"; the replay thread sends the oldest file when the device is
 * online and deletes it. storage.c provides the real file system and MQTT
 * operations through sf_ops_t.
 */

#ifndef STORE_FORWARD_H
#define STORE_FORWARD_H

#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Size of the buffers holding a stored file's name. */
#define SF_NAME_MAX 256

/** @brief Operations on the storage and the network, provided by the caller. */
typedef struct
{
    void *ctx;   /**< Passed to every callback. */
    /** 1 when MQTT is connected and the internet is reachable. */
    int   (*connected)(void *ctx);
    /** Calls each(name, each_ctx) for every entry of the storage; returns 0, or -1 if it can't be listed. */
    int   (*list)(void *ctx, void (*each)(const char *name, void *each_ctx), void *each_ctx);
    /** Whole content of a stored file, NUL-terminated and malloc()ed; NULL on failure. */
    char *(*read)(void *ctx, const char *name);
    /** Writes a new file; returns 0 on success. */
    int   (*write)(void *ctx, const char *name, const char *payload);
    /** Publishes a payload (mqtt_publish(): stores it again if publishing fails). */
    void  (*publish)(void *ctx, const char *payload);
    /** Deletes a stored file; returns 0 on success. */
    int   (*remove)(void *ctx, const char *name);
} sf_ops_t;

/** @brief Outcome of one replay attempt. */
typedef enum
{
    SF_NOT_CONNECTED    = 0,  /**< Offline: nothing done. */
    SF_LIST_FAILED      = 1,  /**< The storage could not be listed. */
    SF_NOTHING_PENDING  = 2,  /**< No stored payloads. */
    SF_READ_FAILED      = 3,  /**< The oldest file could not be read (kept). */
    SF_SENT             = 4,  /**< Oldest payload published and its file deleted. */
    SF_SENT_NOT_REMOVED = 5   /**< Published, but deleting the file failed. */
} sf_replay_result_t;

/**
 * @brief 1 if a storage entry is a stored payload (name ends in ".txt").
 *
 * @param name File name.
 * @return 1 or 0.
 */
int sf_is_payload_file(const char *name);

/**
 * @brief Stores a payload as "<now * 1000>.txt".
 *
 * @param ops     Storage operations.
 * @param payload NUL-terminated payload (NULL: nothing stored).
 * @param now     Current Unix time in seconds.
 * @param name    Output: the file name used.
 * @return 0 on success, -1 if payload is NULL or the write failed.
 */
int sf_store(const sf_ops_t *ops, const char *payload, time_t now, char name[SF_NAME_MAX]);

/**
 * @brief One replay step: when online, publish the oldest stored payload and delete it.
 *
 * "Oldest" is the smallest file name in strcmp() order. Exactly one file is
 * sent per call; the replay thread calls this every REPLAY_INTERVAL_SEC.
 *
 * @param ops  Storage and network operations.
 * @param name Output: the file that was read / sent (empty if none).
 * @return What happened (sf_replay_result_t).
 */
sf_replay_result_t sf_replay_once(const sf_ops_t *ops, char name[SF_NAME_MAX]);

#ifdef __cplusplus
}
#endif

#endif /* STORE_FORWARD_H */
