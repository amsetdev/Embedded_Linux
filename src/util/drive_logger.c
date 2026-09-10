/**
 * @file drive_logger.c
 * @brief Google Drive log upload implementation.
 *
 * Captures application error and warning messages from stderr,
 * buffers them in memory, and periodically uploads batches to
 * a Google Apps Script endpoint via HTTP POST.
 */

#define _GNU_SOURCE
#include "drive_logger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <curl/curl.h>

static FILE *s_real_stderr = NULL;

#define _IL(lvl, fmt, ...) \
    do { \
        FILE *_f = s_real_stderr ? s_real_stderr : stderr; \
        fprintf(_f, "[" lvl "] drive_logger: " fmt "\n", ##__VA_ARGS__); \
        fflush(_f); \
    } while (0)

#define IL_I(fmt, ...)  _IL("I", fmt, ##__VA_ARGS__)
#define IL_W(fmt, ...)  _IL("W", fmt, ##__VA_ARGS__)
#define IL_E(fmt, ...)  _IL("E", fmt, ##__VA_ARGS__)

typedef struct {
    char line[DRIVE_LOGGER_LOG_LINE_MAX];
    bool is_error;
} log_item_t;


typedef struct {
    log_item_t       items[DRIVE_LOGGER_QUEUE_SIZE];
    int              head, tail, count;
    pthread_mutex_t  lock;
    pthread_cond_t   not_empty;
} log_queue_t;


typedef struct {
    char            *buf;
    size_t           buf_size;
    size_t           position;
    int              log_count;
    bool             is_collecting;
    bool             upload_in_progress;
    bool             upload_successful;
    time_t           last_log_time;
    pthread_mutex_t  lock;
} log_buffer_t;

static log_queue_t  s_queue;
static log_buffer_t s_buf;

static volatile bool s_logger_active = false;
static volatile bool s_task_running  = false;

static int       s_pipe_read_fd = -1;

static pthread_t s_upload_thread;
static pthread_t s_reader_thread;

static volatile uint32_t s_total_captured  = 0;
static volatile uint32_t s_total_errors    = 0;
static volatile uint32_t s_total_warnings  = 0;
static volatile uint32_t s_total_uploaded  = 0;
static volatile uint32_t s_upload_failures = 0;

/**
 * @brief Gets a formatted timestamp string.
 *
 * Writes the current local time into the provided buffer using
 * the format "YYYY-MM-DD HH:MM:SS".
 *
 * @param buf  Output buffer to receive the formatted timestamp.
 * @param len  Size of the output buffer in bytes.
 */
static void get_timestamp(char *buf, size_t len)
{
    time_t t = time(NULL);
    struct tm tm_info;
    localtime_r(&t, &tm_info);
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", &tm_info);
}

/**
 * @brief Gets a compact timestamp string suitable for filenames.
 *
 * Writes the current local time into the provided buffer using
 * the format "YYYYMMDD_HHMMSS" (no spaces or colons).
 *
 * @param buf  Output buffer to receive the compact timestamp.
 * @param len  Size of the output buffer in bytes.
 */
static void get_timestamp_compact(char *buf, size_t len)
{
    time_t t = time(NULL);
    struct tm tm_info;
    localtime_r(&t, &tm_info);
    strftime(buf, len, "%Y%m%d_%H%M%S", &tm_info);
}

/**
 * @brief Strips ANSI escape sequences from a string.
 *
 * Copies the input string to the output buffer while removing
 * any ANSI escape sequences (e.g., color codes). The output
 * is always null-terminated.
 *
 * @param in        Input string potentially containing ANSI sequences.
 * @param out       Output buffer for the cleaned string.
 * @param out_size  Size of the output buffer in bytes.
 */
static void strip_ansi(const char *in, char *out, size_t out_size)
{
    size_t oi = 0, ii = 0;
    while (in[ii] && oi < out_size - 1) {
        if (in[ii] == '\033') {
            while (in[ii] && in[ii] != 'm') ii++;
            if (in[ii] == 'm') ii++;
        } else {
            out[oi++] = in[ii++];
        }
    }
    out[oi] = '\0';
}

/**
 * @brief Classifies a log line as error, warning, or normal.
 *
 * Inspects the line content for known error and warning prefixes,
 * including ANSI-colored variants.
 *
 * @param line  The log line to classify.
 * @return 1 if the line is an error, 2 if a warning, 0 otherwise.
 */
static int classify_line(const char *line)
{
    if (!line || strlen(line) < 3) return 0;

    if ((line[0] == 'E' || line[0] == 'e') && line[1] == ' ') return 1;
    if ((line[0] == 'W' || line[0] == 'w') && line[1] == ' ') return 2;

    if (strstr(line, "\033[0;31mE") || strstr(line, "\033[0;91mE")) return 1;
    if (strstr(line, "\033[0;33mW") || strstr(line, "\033[0;93mW")) return 2;

    if (strncmp(line, "ERROR",   5) == 0 ||
        strncmp(line, "CRIT",    4) == 0 ||
        strncmp(line, "ALERT",   5) == 0 ||
        strncmp(line, "EMERG",   5) == 0) return 1;
    if (strncmp(line, "WARN",    4) == 0 ||
        strncmp(line, "WARNING", 7) == 0) return 2;

    return 0;
}

/**
 * @brief Parses a raw log line into tag, message, and error flag.
 *
 * Strips ANSI escape sequences from the raw input, then extracts
 * the tag (module name) and message body. The error flag is set
 * based on line classification.
 *
 * @param raw           Raw log line, potentially with ANSI codes.
 * @param tag_out       Output buffer for the extracted tag string.
 * @param tag_size      Size of the tag output buffer in bytes.
 * @param msg_out       Pointer set to the start of the message body
 *                      within an internal static buffer.
 * @param is_error_out  Set to true if the line is an error, false otherwise.
 */
static void parse_line(const char *raw,
                        char *tag_out, size_t tag_size,
                        const char **msg_out,
                        bool *is_error_out)
{
    static char clean[DRIVE_LOGGER_LOG_LINE_MAX * 2];
    strip_ansi(raw, clean, sizeof(clean));

    strncpy(tag_out, "UNKNOWN", tag_size - 1);
    tag_out[tag_size - 1] = '\0';
    *msg_out      = clean;
    *is_error_out = (classify_line(raw) == 1);

    const char *paren = strchr(clean, ')');
    if (paren) {
        const char *tag_start = paren + 2;
        const char *colon     = strchr(tag_start, ':');
        if (colon) {
            size_t tlen = (size_t)(colon - tag_start);
            if (tlen > 0 && tlen < tag_size) {
                strncpy(tag_out, tag_start, tlen);
                tag_out[tlen] = '\0';
            }
            *msg_out = (*(colon + 1) == ' ') ? colon + 2 : colon + 1;
            return;
        }
    }
    const char *colon = strchr(clean, ':');
    if (colon)
        *msg_out = (*(colon + 1) == ' ') ? colon + 2 : colon + 1;
}

/**
 * @brief Initializes the log queue.
 *
 * Zeroes the queue structure and initializes its mutex and
 * condition variable.
 *
 * @param q  Pointer to the log queue to initialize.
 */
static void queue_init(log_queue_t *q)
{
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->not_empty, NULL);
}

/**
 * @brief Pushes a log item onto the queue.
 *
 * Copies the item into the queue's ring buffer and signals
 * the condition variable. If the queue is full the item is
 * dropped.
 *
 * @param q     Pointer to the log queue.
 * @param item  Pointer to the log item to enqueue.
 * @return true if the item was enqueued, false if the queue was full.
 */
static bool queue_push(log_queue_t *q, const log_item_t *item)
{
    pthread_mutex_lock(&q->lock);
    if (q->count >= DRIVE_LOGGER_QUEUE_SIZE) {
        pthread_mutex_unlock(&q->lock);
        return false;
    }
    q->items[q->tail] = *item;
    q->tail  = (q->tail + 1) % DRIVE_LOGGER_QUEUE_SIZE;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
    return true;
}

/**
 * @brief Pops a log item from the queue with a timeout.
 *
 * Blocks until an item is available or the timeout expires.
 * On success the item is copied to the caller's buffer.
 *
 * @param q           Pointer to the log queue.
 * @param item        Output buffer for the dequeued log item.
 * @param timeout_ms  Maximum time to wait in milliseconds.
 * @return true if an item was dequeued, false on timeout.
 */
static bool queue_pop(log_queue_t *q, log_item_t *item, int timeout_ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec  += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }

    pthread_mutex_lock(&q->lock);
    while (q->count == 0) {
        int rc = pthread_cond_timedwait(&q->not_empty, &q->lock, &ts);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&q->lock);
            return false;
        }
    }
    *item   = q->items[q->head];
    q->head = (q->head + 1) % DRIVE_LOGGER_QUEUE_SIZE;
    q->count--;
    pthread_mutex_unlock(&q->lock);
    return true;
}

/**
 * @brief Destroys the log queue.
 *
 * Releases the mutex and condition variable resources
 * associated with the queue.
 *
 * @param q  Pointer to the log queue to destroy.
 */
static void queue_destroy(log_queue_t *q)
{
    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->not_empty);
}

/**
 * @brief Initializes the log buffer.
 *
 * Allocates the initial buffer memory, writes the log file
 * header, and sets the buffer to collecting state.
 *
 * @param b  Pointer to the log buffer to initialize.
 * @return true on success, false if memory allocation fails.
 */
static bool buf_init(log_buffer_t *b)
{
    memset(b, 0, sizeof(*b));
    pthread_mutex_init(&b->lock, NULL);

    b->buf_size = 4096;
    b->buf = malloc(b->buf_size);
    if (!b->buf) { IL_E("malloc failed"); return false; }

    b->is_collecting = true;

    char ts[32];
    get_timestamp(ts, sizeof(ts));
    b->position = (size_t)snprintf(b->buf, b->buf_size,
        "============================================\n"
        "STM32MP1 LOGS  (ERRORS & WARNINGS)\n"
        "Device  : %s\n"
        "Started : %s\n"
        "============================================\n\n",
        DRIVE_LOGGER_DEVICE_ID, ts);

    IL_I("Buffer ready — collecting up to %d logs.", DRIVE_LOGGER_TARGET_LOG_COUNT);
    return true;
}

/**
 * @brief Adds a log entry to the buffer.
 *
 * Appends a formatted log line to the buffer, growing it if
 * necessary. Stops collecting once the target log count is
 * reached.
 *
 * @param b         Pointer to the log buffer.
 * @param tag       Module or subsystem tag for the log entry.
 * @param message   The log message text.
 * @param is_error  true if the entry is an error, false for a warning.
 * @return true if the entry was added, false if the buffer is full,
 *         an upload is in progress, or memory allocation fails.
 */
static bool buf_add(log_buffer_t *b, const char *tag,
                    const char *message, bool is_error)
{
    pthread_mutex_lock(&b->lock);

    if (!b->is_collecting || b->upload_in_progress ||
        b->log_count >= DRIVE_LOGGER_TARGET_LOG_COUNT) {
        pthread_mutex_unlock(&b->lock);
        return false;
    }

    const char *type = is_error ? "[ERROR]" : "[WARNING]";
    int needed = snprintf(NULL, 0, "%s %s: %s\n", type, tag, message);

    if (b->position + (size_t)needed + 1 >= b->buf_size) {
        size_t new_size = b->buf_size * 2;
        if (new_size > DRIVE_LOGGER_MAX_BUFFER_SIZE) {
            IL_E("Max buffer size reached.");
            pthread_mutex_unlock(&b->lock);
            return false;
        }
        char *nb = realloc(b->buf, new_size);
        if (!nb) {
            IL_E("realloc failed.");
            pthread_mutex_unlock(&b->lock);
            return false;
        }
        b->buf      = nb;
        b->buf_size = new_size;
    }

    int written = snprintf(b->buf + b->position,
                           b->buf_size - b->position,
                           "%s %s: %s\n", type, tag, message);
    if (written > 0) {
        b->position += (size_t)written;
        b->log_count++;
        b->last_log_time = time(NULL);

        if (is_error) s_total_errors++;
        else          s_total_warnings++;

        IL_I("Buffered %s #%d — tag=%s",
             is_error ? "ERROR" : "WARNING", b->log_count, tag);

        if (b->log_count >= DRIVE_LOGGER_TARGET_LOG_COUNT) {
            IL_I("Target %d reached — stopping collection.", DRIVE_LOGGER_TARGET_LOG_COUNT);
            b->is_collecting = false;
        }
    }

    pthread_mutex_unlock(&b->lock);
    return (written > 0);
}

/**
 * @brief Checks if the buffer is ready for upload.
 *
 * The buffer is considered ready when the target log count has
 * been reached or when the inactivity timeout has elapsed since
 * the last log entry.
 *
 * @param b  Pointer to the log buffer.
 * @return true if the buffer should be uploaded, false otherwise.
 */
static bool buf_ready(log_buffer_t *b)
{
    pthread_mutex_lock(&b->lock);
    bool ready = false;

    if (b->log_count >= DRIVE_LOGGER_TARGET_LOG_COUNT &&
        !b->upload_in_progress && !b->upload_successful) {
        ready = true;
    } else if (b->log_count > 0 && b->last_log_time > 0 &&
               !b->upload_in_progress && !b->upload_successful) {
        time_t idle = time(NULL) - b->last_log_time;
        if (idle >= DRIVE_LOGGER_INACTIVITY_TIMEOUT_S) {
            IL_I("Idle %lds — triggering upload of %d logs.", (long)idle, b->log_count);
            ready = true;
        }
    }

    pthread_mutex_unlock(&b->lock);
    return ready;
}

/**
 * @brief Prepares the buffer content for upload.
 *
 * Copies the current buffer contents into a newly allocated
 * string and marks the buffer as upload-in-progress.
 *
 * @param b          Pointer to the log buffer.
 * @param out        Receives a pointer to a malloc'd copy of the buffer
 *                   content. The caller must free this memory.
 * @param count_out  Receives the number of log entries in the copy.
 * @return true if the upload copy was prepared, false if the buffer
 *         is empty, already uploading, or memory allocation fails.
 */
static bool buf_begin_upload(log_buffer_t *b, char **out, int *count_out)
{
    pthread_mutex_lock(&b->lock);

    if (b->log_count < 1 || b->upload_in_progress || b->upload_successful) {
        pthread_mutex_unlock(&b->lock);
        return false;
    }

    b->upload_in_progress = true;
    *out = malloc(b->position + 1);
    if (!*out) {
        IL_E("malloc for upload copy failed.");
        b->upload_in_progress = false;
        pthread_mutex_unlock(&b->lock);
        return false;
    }
    memcpy(*out, b->buf, b->position + 1);
    *count_out = b->log_count;

    IL_I("Upload copy ready: %d logs, %zu bytes.", b->log_count, b->position);
    pthread_mutex_unlock(&b->lock);
    return true;
}

/**
 * @brief Marks the current upload as successful.
 *
 * Clears the upload-in-progress flag and sets the upload-successful
 * flag so the buffer can be reset for the next batch.
 *
 * @param b  Pointer to the log buffer.
 */
static void buf_upload_ok(log_buffer_t *b)
{
    pthread_mutex_lock(&b->lock);
    b->upload_in_progress = false;
    b->upload_successful  = true;
    pthread_mutex_unlock(&b->lock);
}

/**
 * @brief Marks the current upload as failed.
 *
 * Clears the upload-in-progress flag while leaving the buffer
 * content intact so the upload can be retried.
 *
 * @param b  Pointer to the log buffer.
 */
static void buf_upload_fail(log_buffer_t *b)
{
    pthread_mutex_lock(&b->lock);
    b->upload_in_progress = false;
    b->upload_successful  = false;
    pthread_mutex_unlock(&b->lock);
}

/**
 * @brief Resets the buffer for the next batch.
 *
 * Clears all buffer content and state, writes a new batch header,
 * and resumes log collection.
 *
 * @param b  Pointer to the log buffer to reset.
 */
static void buf_reset(log_buffer_t *b)
{
    pthread_mutex_lock(&b->lock);

    memset(b->buf, 0, b->buf_size);
    b->log_count          = 0;
    b->position           = 0;
    b->upload_in_progress = false;
    b->upload_successful  = false;
    b->is_collecting      = true;
    b->last_log_time      = 0;

    char ts[32];
    get_timestamp(ts, sizeof(ts));
    b->position = (size_t)snprintf(b->buf, b->buf_size,
        "============================================\n"
        "STM32MP1 LOGS — NEW BATCH (ERRORS & WARNINGS)\n"
        "Device     : %s\n"
        "Batch Start: %s\n"
        "============================================\n\n",
        DRIVE_LOGGER_DEVICE_ID, ts);

    IL_I("Buffer reset — ready for next batch.");
    pthread_mutex_unlock(&b->lock);
}

/**
 * @brief Destroys the log buffer.
 *
 * Frees the dynamically allocated buffer memory and destroys
 * the associated mutex.
 *
 * @param b  Pointer to the log buffer to destroy.
 */
static void buf_destroy(log_buffer_t *b)
{
    pthread_mutex_lock(&b->lock);
    free(b->buf);
    b->buf = NULL;
    pthread_mutex_unlock(&b->lock);
    pthread_mutex_destroy(&b->lock);
}

/**
 * @brief URL-encodes a string.
 *
 * Allocates and returns a new string where unsafe characters are
 * percent-encoded. Spaces are encoded as '+'. The caller must
 * free the returned string.
 *
 * @param str  The input string to encode.
 * @return A newly allocated URL-encoded string, or NULL on failure.
 */
static char *url_encode(const char *str)
{
    if (!str) return NULL;
    size_t len = strlen(str);
    char *enc  = malloc(len * 3 + 1);
    if (!enc) return NULL;

    int pos = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)str[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            enc[pos++] = (char)c;
        } else if (c == ' ') {
            enc[pos++] = '+';
        } else {
            sprintf(&enc[pos], "%%%02X", c);
            pos += 3;
        }
    }
    enc[pos] = '\0';
    return enc;
}

/* -----------------------------------------------------------------------
 * HTTP upload
 * --------------------------------------------------------------------- */

/**
 * @brief libcurl write callback that discards response data.
 *
 * Used as the CURLOPT_WRITEFUNCTION to suppress output from
 * HTTP responses.
 *
 * @param ptr   Pointer to the received data.
 * @param sz    Size of each data element.
 * @param nmemb Number of data elements.
 * @param ud    User data pointer (unused).
 * @return The total number of bytes handled (sz * nmemb).
 */
static size_t discard_response(void *ptr, size_t sz, size_t nmemb, void *ud)
{
    (void)ptr; (void)ud;
    return sz * nmemb;
}

/**
 * @brief Uploads log content via HTTP POST.
 *
 * URL-encodes the log content and filename, constructs a
 * form-encoded POST body, and sends it to the configured
 * Google Apps Script endpoint using libcurl. Retries up to
 * DRIVE_LOGGER_MAX_RETRIES times on failure.
 *
 * @param content  The log text to upload.
 * @param count    Number of log entries contained in the content.
 * @return true if the upload succeeded (HTTP 2xx/3xx), false otherwise.
 */
static bool http_upload(const char *content, int count)
{
    if (!content || count == 0) return false;

    char ts_compact[24];
    get_timestamp_compact(ts_compact, sizeof(ts_compact));

    char filename[128];
    snprintf(filename, sizeof(filename),
             "LOGS_%s_%s.txt", DRIVE_LOGGER_DEVICE_ID, ts_compact);

    IL_I("Uploading %d logs → %s", count, filename);

    char *enc_name = url_encode(filename);
    char *enc_body = url_encode(content);
    if (!enc_name || !enc_body) {
        IL_E("URL encoding failed.");
        free(enc_name); free(enc_body);
        return false;
    }

    size_t post_len = strlen(enc_name) + strlen(enc_body) + 80;
    char  *post = malloc(post_len);
    if (!post) {
        IL_E("malloc for POST data failed.");
        free(enc_name); free(enc_body);
        return false;
    }
    snprintf(post, post_len,
             "fileName=%s&fileContent=%s&device=%s&logCount=%d",
             enc_name, enc_body, DRIVE_LOGGER_DEVICE_ID, count);
    free(enc_name);
    free(enc_body);

    bool success = false;

    CURL *curl = curl_easy_init();
    if (!curl) { IL_E("curl_easy_init failed."); free(post); return false; }

    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/x-www-form-urlencoded");

    curl_easy_setopt(curl, CURLOPT_URL,            DRIVE_LOGGER_SERVER_URL);
    curl_easy_setopt(curl, CURLOPT_POST,            1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     post);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     hdrs);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        (long)DRIVE_LOGGER_UPLOAD_TIMEOUT_S);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  discard_response);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,      "STM32MP1-DriveLogger/1.0");

    for (int attempt = 0; attempt <= DRIVE_LOGGER_MAX_RETRIES && !success; attempt++) {
        CURLcode rc = curl_easy_perform(curl);
        if (rc == CURLE_OK) {
            long http_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            if (http_code >= 200 && http_code < 400) {
                IL_I("Upload OK (HTTP %ld).", http_code);
                success = true;
                s_total_uploaded += (uint32_t)count;
            } else {
                IL_W("HTTP %ld on attempt %d.", http_code, attempt + 1);
            }
        } else {
            IL_W("curl error on attempt %d: %s", attempt + 1, curl_easy_strerror(rc));
        }
        if (!success && attempt < DRIVE_LOGGER_MAX_RETRIES) sleep(2);
    }

    if (!success) s_upload_failures++;

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    free(post);
    return success;
}

/**
 * @brief Reader thread that captures stderr output.
 *
 * Reads characters from the intercepted stderr pipe one byte at
 * a time, echoes them to the real stderr, and enqueues complete
 * lines classified as errors or warnings into the log queue.
 *
 * @param arg  Thread argument (unused).
 * @return Always returns NULL.
 */
static void *reader_thread_fn(void *arg)
{
    (void)arg;
    char line[DRIVE_LOGGER_LOG_LINE_MAX];
    int  pos = 0;

    IL_I("Reader thread started (fd=%d).", s_pipe_read_fd);

    while (s_task_running) {
        char ch;
        ssize_t n = read(s_pipe_read_fd, &ch, 1);
        if (n <= 0) { usleep(500); continue; }

        /* Echo to real stderr */
        if (s_real_stderr) {
            fputc(ch, s_real_stderr);
            if (ch == '\n') fflush(s_real_stderr);
        }

        if (ch == '\n' || pos >= DRIVE_LOGGER_LOG_LINE_MAX - 1) {
            line[pos] = '\0';

            int lt = classify_line(line);
            if ((lt == 1 || lt == 2) && s_logger_active) {
                log_item_t item;
                strncpy(item.line, line, DRIVE_LOGGER_LOG_LINE_MAX - 1);
                item.line[DRIVE_LOGGER_LOG_LINE_MAX - 1] = '\0';
                item.is_error = (lt == 1);
                if (queue_push(&s_queue, &item))
                    s_total_captured++;
            }
            pos = 0;
        } else {
            line[pos++] = ch;
        }
    }

    IL_I("Reader thread exiting.");
    return NULL;
}

/**
 * @brief Upload thread that processes and uploads log batches.
 *
 * Continuously dequeues log items, parses them, and adds them to
 * the buffer. When the buffer is ready (target count reached or
 * inactivity timeout), prepares and uploads the batch via HTTP.
 * Resets the buffer on success or retries on failure.
 *
 * @param arg  Thread argument (unused).
 * @return Always returns NULL.
 */
static void *upload_thread_fn(void *arg)
{
    (void)arg;
    IL_I("Upload thread started. Collect %d logs → POST → reset (or POST after %ds idle).",
         DRIVE_LOGGER_TARGET_LOG_COUNT, DRIVE_LOGGER_INACTIVITY_TIMEOUT_S);

    log_item_t  item;
    char        tag[64];
    const char *msg;
    bool        is_error;

    while (s_task_running) {
        if (queue_pop(&s_queue, &item, 100)) {
            parse_line(item.line, tag, sizeof(tag), &msg, &is_error);
            if (strcmp(tag, "drive_logger") == 0) continue;   /* skip self */
            buf_add(&s_buf, tag, msg, is_error);
        }

        if (buf_ready(&s_buf)) {
            char *content = NULL;
            int   count   = 0;

            if (buf_begin_upload(&s_buf, &content, &count)) {
                bool ok = http_upload(content, count);
                free(content);

                if (ok) {
                    buf_upload_ok(&s_buf);
                    sleep(1);
                    buf_reset(&s_buf);
                    IL_I("Stats — captured:%" PRIu32 " errors:%" PRIu32
                         " warnings:%" PRIu32 " uploaded:%" PRIu32
                         " failures:%" PRIu32,
                         s_total_captured, s_total_errors,
                         s_total_warnings, s_total_uploaded, s_upload_failures);
                } else {
                    buf_upload_fail(&s_buf);
                    IL_W("Upload failed — retrying in 5 s.");
                    sleep(5);
                }
            }
        }
    }

    IL_I("Upload thread exiting.");
    return NULL;
}

/**
 * @brief Starts the drive logger service.
 *
 * Saves the real stderr, creates a pipe to intercept stderr output,
 * initializes libcurl, the log queue, and the log buffer, then
 * spawns the reader and upload background threads.
 *
 * @return true if the logger started successfully, false on failure.
 */
bool drive_logger_start(void)
{
    if (s_logger_active) {
        IL_W("Already running.");
        return true;
    }

    IL_I("Starting drive logger...");

    /* Save the real stderr before redirecting */
    int saved_fd = dup(STDERR_FILENO);
    if (saved_fd < 0) { perror("drive_logger: dup"); return false; }

    s_real_stderr = fdopen(saved_fd, "w");
    if (!s_real_stderr) { perror("drive_logger: fdopen"); close(saved_fd); return false; }
    setbuf(s_real_stderr, NULL);

    /* Create interception pipe */
    int pipefd[2];
    if (pipe(pipefd) < 0) { perror("drive_logger: pipe"); return false; }
    s_pipe_read_fd = pipefd[0];

    /* Redirect stderr → write-end of pipe */
    if (dup2(pipefd[1], STDERR_FILENO) < 0) {
        perror("drive_logger: dup2");
        close(pipefd[0]); close(pipefd[1]);
        return false;
    }
    close(pipefd[1]);

    curl_global_init(CURL_GLOBAL_DEFAULT);
    queue_init(&s_queue);
    if (!buf_init(&s_buf)) return false;

    s_task_running = true;

    if (pthread_create(&s_reader_thread, NULL, reader_thread_fn, NULL) != 0) {
        IL_E("Failed to create reader thread.");
        s_task_running = false;
        return false;
    }

    if (pthread_create(&s_upload_thread, NULL, upload_thread_fn, NULL) != 0) {
        IL_E("Failed to create upload thread.");
        s_task_running = false;
        pthread_join(s_reader_thread, NULL);
        return false;
    }

    s_logger_active = true;
    IL_I("Drive logger active — two background threads running.");
    return true;
}

/**
 * @brief Stops the drive logger service.
 *
 * Signals the background threads to exit, joins them, destroys
 * the queue and buffer resources, and cleans up libcurl.
 */
void drive_logger_stop(void)
{
    if (!s_logger_active) return;
    IL_I("Stopping drive logger...");

    s_logger_active = false;
    s_task_running  = false;
    pthread_cond_signal(&s_queue.not_empty);

    pthread_join(s_reader_thread, NULL);
    pthread_join(s_upload_thread, NULL);

    queue_destroy(&s_queue);
    buf_destroy(&s_buf);
    curl_global_cleanup();

    IL_I("Stopped. Final stats — captured:%" PRIu32
         " uploaded:%" PRIu32 " failures:%" PRIu32,
         s_total_captured, s_total_uploaded, s_upload_failures);
}

/**
 * @brief Writes a formatted log message.
 *
 * Formats the message with the given level prefix and tag, then
 * writes it to stderr where it will be intercepted by the reader
 * thread for buffering and upload.
 *
 * @param level  Log severity level (DRIVE_LOG_ERROR, DRIVE_LOG_WARNING,
 *               or DRIVE_LOG_INFO).
 * @param tag    Module or subsystem name.
 * @param fmt    printf-style format string.
 * @param ...    Format arguments.
 */
void drive_logger_write(int level, const char *tag, const char *fmt, ...)
{
    const char *prefix = (level == DRIVE_LOG_ERROR)  ? "E" :
                         (level == DRIVE_LOG_WARNING) ? "W" : "I";
    char msg[DRIVE_LOGGER_LOG_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);


    fprintf(stderr, "%s (%s) %s: %s\n", prefix, tag, tag, msg);
    fflush(stderr);
}

/**
 * @brief Retrieves current logger statistics.
 *
 * Copies the current values of all internal counters into the
 * provided statistics structure.
 *
 * @param stats  Pointer to a drive_logger_stats_t structure to populate.
 *               If NULL, no action is taken.
 */
void drive_logger_get_stats(drive_logger_stats_t *stats)
{
    if (!stats) return;
    stats->total_captured  = s_total_captured;
    stats->total_errors    = s_total_errors;
    stats->total_warnings  = s_total_warnings;
    stats->total_uploaded  = s_total_uploaded;
    stats->upload_failures = s_upload_failures;
}

/**
 * @brief Checks if the drive logger is currently running.
 *
 * @return true if the logger is active, false otherwise.
 */
bool drive_logger_is_running(void)
{
    return s_logger_active;
}
