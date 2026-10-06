/**
 * @file store_forward.c
 * @brief Store-and-forward decisions (see store_forward.h).
 *
 * Moved out of storage.c (offline_store(), replay_worker()) unchanged in
 * behaviour so it can be host-tested.
 */

#include "store_forward.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Documented in store_forward.h. */
int sf_is_payload_file(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && strcmp(dot, ".txt") == 0;
}

/* Documented in store_forward.h. */
int sf_store(const sf_ops_t *ops, const char *payload, time_t now, char name[SF_NAME_MAX])
{
    name[0] = '\0';
    if (!payload)
        return -1;

    long long ms = (long long)now * 1000;
    snprintf(name, SF_NAME_MAX, "%lld.txt", ms);

    return ops->write(ops->ctx, name, payload) == 0 ? 0 : -1;
}

/**
 * @brief sf_ops_t.list callback: keeps the smallest payload file name.
 * @param name Storage entry.
 * @param each_ctx char[SF_NAME_MAX] holding the oldest name so far.
 */
static void keep_oldest(const char *name, void *each_ctx)
{
    char *oldest = each_ctx;

    if (!sf_is_payload_file(name))
        return;

    if (oldest[0] == '\0' || strcmp(name, oldest) < 0)
    {
        strncpy(oldest, name, SF_NAME_MAX - 1);
        oldest[SF_NAME_MAX - 1] = '\0';
    }
}

/* Documented in store_forward.h. */
sf_replay_result_t sf_replay_once(const sf_ops_t *ops, char name[SF_NAME_MAX])
{
    name[0] = '\0';

    if (!ops->connected(ops->ctx))
        return SF_NOT_CONNECTED;

    if (ops->list(ops->ctx, keep_oldest, name) != 0)
        return SF_LIST_FAILED;

    if (name[0] == '\0')
        return SF_NOTHING_PENDING;

    char *buf = ops->read(ops->ctx, name);
    if (!buf)
        return SF_READ_FAILED;

    ops->publish(ops->ctx, buf);
    free(buf);

    return ops->remove(ops->ctx, name) == 0 ? SF_SENT : SF_SENT_NOT_REMOVED;
}
