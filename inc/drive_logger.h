#ifndef DRIVE_LOGGER_H
#define DRIVE_LOGGER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>


#ifndef DRIVE_LOGGER_DEVICE_ID
#  define DRIVE_LOGGER_DEVICE_ID        "SIR68b29"
#endif

#ifndef DRIVE_LOGGER_SERVER_URL
#  define DRIVE_LOGGER_SERVER_URL \
    "https://script.google.com/macros/s/AKfycbxvcQLH6a9cNV5TXhL155ofyY7jTOq6lZiFGjB_ZykO2B2XG8WpTWjhHrqc8fJQt2pUCw/exec"
#endif

#ifndef DRIVE_LOGGER_TARGET_LOG_COUNT
#  define DRIVE_LOGGER_TARGET_LOG_COUNT  10   
#endif

#ifndef DRIVE_LOGGER_INACTIVITY_TIMEOUT_S
#  define DRIVE_LOGGER_INACTIVITY_TIMEOUT_S  5 
#endif

#ifndef DRIVE_LOGGER_QUEUE_SIZE
#  define DRIVE_LOGGER_QUEUE_SIZE        50
#endif

#ifndef DRIVE_LOGGER_LOG_LINE_MAX
#  define DRIVE_LOGGER_LOG_LINE_MAX      256
#endif

#ifndef DRIVE_LOGGER_MAX_BUFFER_SIZE
#  define DRIVE_LOGGER_MAX_BUFFER_SIZE   (16 * 1024)
#endif

#ifndef DRIVE_LOGGER_UPLOAD_TIMEOUT_S
#  define DRIVE_LOGGER_UPLOAD_TIMEOUT_S  45
#endif

#ifndef DRIVE_LOGGER_MAX_RETRIES
#  define DRIVE_LOGGER_MAX_RETRIES       2
#endif


#define DRIVE_LOG_ERROR    1
#define DRIVE_LOG_WARNING  2
#define DRIVE_LOG_INFO     3  


typedef struct {
    uint32_t total_captured;  
    uint32_t total_errors;     
    uint32_t total_warnings;  
    uint32_t total_uploaded; 
    uint32_t upload_failures;  
} drive_logger_stats_t;

bool drive_logger_start(void);

void drive_logger_stop(void);


void drive_logger_write(int level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

void drive_logger_get_stats(drive_logger_stats_t *stats);

bool drive_logger_is_running(void);

#define DL_LOGE(tag, fmt, ...)  drive_logger_write(DRIVE_LOG_ERROR,   tag, fmt, ##__VA_ARGS__)
#define DL_LOGW(tag, fmt, ...)  drive_logger_write(DRIVE_LOG_WARNING,  tag, fmt, ##__VA_ARGS__)
#define DL_LOGI(tag, fmt, ...)  drive_logger_write(DRIVE_LOG_INFO,     tag, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif