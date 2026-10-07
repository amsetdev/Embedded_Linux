/**
 * @file store_forward.h
 * @brief Store-and-forward decisions for telemetry that could not be published
 *        (no file system or MQTT dependency, host-tested).
 *
 * A payload that can't be published is stored as one file named
 * "\<unix ms, 13 digits\>_\<sequence\>.txt" (older firmware wrote "\<unix ms\>.txt";
 * both sort oldest-first). When the device is online the replay thread sends
 * the oldest files and deletes each one only after the broker confirmed it. storage.c provides the real file system and MQTT
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

/** @brief Most stored payloads sent per replay round (rounds are REPLAY_INTERVAL_SEC apart). */
#define SF_REPLAY_BATCH 100

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
    /** Writes a NEW file (must fail rather than overwrite); returns 0 on success. */
    int   (*write)(void *ctx, const char *name, const char *payload);
    /** Publishes a payload; returns 0 only once the broker confirmed it (QoS 1 PUBACK). */
    int   (*publish)(void *ctx, const char *payload);
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
    SF_SENT_NOT_REMOVED = 5,  /**< Published, but deleting the file failed. */
    SF_PUBLISH_FAILED   = 6   /**< Not confirmed by the broker: the file is kept. */
} sf_replay_result_t;

/**
 * @brief 1 if a storage entry is a stored payload (name ends in ".txt").
 *
 * @param name File name.
 * @return 1 or 0.
 */
int sf_is_payload_file(const char *name);

/**
 * @brief Stores a payload as "\<now_ms, 13 digits\>_\<6-digit sequence\>.txt".
 *
 * The sequence number makes names unique even for several payloads in the same
 * millisecond, and keeps them in storing order.
 *
 * @param ops     Storage operations.
 * @param payload NUL-terminated payload (NULL: nothing stored).
 * @param now_ms  Current Unix time in milliseconds.
 * @param name    Output: the file name used.
 * @return 0 on success, -1 if payload is NULL or the write failed.
 */
int sf_store(const sf_ops_t *ops, const char *payload, long long now_ms, char name[SF_NAME_MAX]);

/**
 * @brief One replay step: when online, publish the oldest stored payload and delete it.
 *
 * "Oldest" is the smallest file name in strcmp() order. The file is deleted only
 * when ops->publish() reports the broker's confirmation; otherwise it stays and
 * SF_PUBLISH_FAILED is returned.
 *
 * @param ops  Storage and network operations.
 * @param name Output: the file that was read / sent (empty if none).
 * @return What happened (sf_replay_result_t).
 */
sf_replay_result_t sf_replay_once(const sf_ops_t *ops, char name[SF_NAME_MAX]);

/**
 * @brief One replay round: sf_replay_once() until nothing is pending, a step
 *        fails, or max_files were sent.
 *
 * @param ops       Storage and network operations.
 * @param max_files Most files to send (SF_REPLAY_BATCH in the firmware).
 * @param last      Output: result of the last step (why the round ended).
 * @param name      Output: file of the last step.
 * @return Number of payloads sent (and deleted or not).
 */
int sf_replay(const sf_ops_t *ops, int max_files, sf_replay_result_t *last, char name[SF_NAME_MAX]);

#ifdef __cplusplus
}
#endif

#endif /* STORE_FORWARD_H */
