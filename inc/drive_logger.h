/**
 * @file drive_logger.h
 * @brief Google Drive log upload interface.
 *
 * This module captures application log messages, buffers them in memory,
 * and periodically uploads them to a Google Apps Script endpoint. It
 * provides thread-safe logging functions, runtime statistics, and
 * configurable upload behavior.
 */

#ifndef DRIVE_LOGGER_H
#define DRIVE_LOGGER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

/* -------------------------------------------------------------------------- */
/* Configuration                                                               */
/* -------------------------------------------------------------------------- */

/**
 * @brief Device identifier included with uploaded log messages.
 */
#ifndef DRIVE_LOGGER_DEVICE_ID
#define DRIVE_LOGGER_DEVICE_ID "SIR68b29"
#endif

/**
 * @brief Google Apps Script endpoint used for log uploads.
 */
#ifndef DRIVE_LOGGER_SERVER_URL
#define DRIVE_LOGGER_SERVER_URL \
    "https://script.google.com/macros/s/AKfycbxvcQLH6a9cNV5TXhL155ofyY7jTOq6lZiFGjB_ZykO2B2XG8WpTWjhHrqc8fJQt2pUCw/exec"
#endif

/**
 * @brief Number of log entries to accumulate before uploading.
 */
#ifndef DRIVE_LOGGER_TARGET_LOG_COUNT
#define DRIVE_LOGGER_TARGET_LOG_COUNT 10
#endif

/**
 * @brief Maximum idle time before pending logs are uploaded.
 */
#ifndef DRIVE_LOGGER_INACTIVITY_TIMEOUT_S
#define DRIVE_LOGGER_INACTIVITY_TIMEOUT_S 5
#endif

/**
 * @brief Maximum number of queued log messages.
 */
#ifndef DRIVE_LOGGER_QUEUE_SIZE
#define DRIVE_LOGGER_QUEUE_SIZE 50
#endif

/**
 * @brief Maximum length of a single formatted log message.
 */
#ifndef DRIVE_LOGGER_LOG_LINE_MAX
#define DRIVE_LOGGER_LOG_LINE_MAX 256
#endif

/**
 * @brief Maximum size of the upload buffer.
 */
#ifndef DRIVE_LOGGER_MAX_BUFFER_SIZE
#define DRIVE_LOGGER_MAX_BUFFER_SIZE (16 * 1024)
#endif

/**
 * @brief HTTP upload timeout in seconds.
 */
#ifndef DRIVE_LOGGER_UPLOAD_TIMEOUT_S
#define DRIVE_LOGGER_UPLOAD_TIMEOUT_S 45
#endif

/**
 * @brief Maximum upload retry attempts.
 */
#ifndef DRIVE_LOGGER_MAX_RETRIES
#define DRIVE_LOGGER_MAX_RETRIES 2
#endif

/* -------------------------------------------------------------------------- */
/* Log Levels                                                                  */
/* -------------------------------------------------------------------------- */

/** @brief Error log level. */
#define DRIVE_LOG_ERROR    1

/** @brief Warning log level. */
#define DRIVE_LOG_WARNING  2

/** @brief Informational log level. */
#define DRIVE_LOG_INFO     3

/* -------------------------------------------------------------------------- */
/* Statistics                                                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief Drive logger runtime statistics.
 */
typedef struct
{
    /** Total log messages captured. */
    uint32_t total_captured;

    /** Total error messages captured. */
    uint32_t total_errors;

    /** Total warning messages captured. */
    uint32_t total_warnings;

    /** Total log messages successfully uploaded. */
    uint32_t total_uploaded;

    /** Total upload failures. */
    uint32_t upload_failures;

} drive_logger_stats_t;

/* -------------------------------------------------------------------------- */
/* API                                                                         */
/* -------------------------------------------------------------------------- */

/**
 * @brief Starts the Drive logger service.
 *
 * Initializes internal resources and starts the background
 * upload thread.
 *
 * @return
 * - true if the logger started successfully.
 * - false on failure.
 */
bool drive_logger_start(void);

/**
 * @brief Stops the Drive logger service.
 *
 * Flushes pending logs if necessary and terminates the
 * background upload thread.
 */
void drive_logger_stop(void);

/**
 * @brief Writes a formatted log message.
 *
 * The message is queued for upload to the configured
 * Google Drive logging endpoint.
 *
 * @param level Log severity level.
 * @param tag Module or subsystem name.
 * @param fmt printf-style format string.
 * @param ... Format arguments.
 */
void drive_logger_write(int level,
                        const char *tag,
                        const char *fmt,
                        ...)
    __attribute__((format(printf, 3, 4)));

/**
 * @brief Retrieves current logger statistics.
 *
 * @param stats Pointer to the statistics structure to populate.
 */
void drive_logger_get_stats(drive_logger_stats_t *stats);

/**
 * @brief Checks whether the logger is running.
 *
 * @return
 * - true if the logger is active.
 * - false otherwise.
 */
bool drive_logger_is_running(void);

/* -------------------------------------------------------------------------- */
/* Convenience Logging Macros                                                  */
/* -------------------------------------------------------------------------- */

/**
 * @brief Log an error message.
 */
#define DL_LOGE(tag, fmt, ...) \
    drive_logger_write(DRIVE_LOG_ERROR, tag, fmt, ##__VA_ARGS__)

/**
 * @brief Log a warning message.
 */
#define DL_LOGW(tag, fmt, ...) \
    drive_logger_write(DRIVE_LOG_WARNING, tag, fmt, ##__VA_ARGS__)

/**
 * @brief Log an informational message.
 */
#define DL_LOGI(tag, fmt, ...) \
    drive_logger_write(DRIVE_LOG_INFO, tag, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* DRIVE_LOGGER_H */