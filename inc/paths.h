/**
 * @file paths.h
 * @brief Where the application finds its configuration and keeps its data.
 *
 * Production (deploy/gateway.service):
 *   gateway --config /etc/gateway/smart_rtu_config.json --data-dir /var/lib/gateway
 * Without options (legacy installs started from their own directory):
 *   config "smart_rtu_config.json" and data in the working directory.
 *
 * Data files: \<data dir\>/storage/ (offline buffer), \<data dir\>/modbus_tcp.db,
 * \<data dir\>/ota/ (default OTA download directory).
 */

#ifndef PATHS_H
#define PATHS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Configuration file used when --config is not given (relative to the working directory). */
#define PATHS_DEFAULT_CONFIG   "smart_rtu_config.json"

/** @brief Data directory used when --data-dir is not given. */
#define PATHS_DEFAULT_DATA_DIR "."

/** @brief Longest supported path. */
#define PATHS_MAX 256

/**
 * @brief Sets the configuration file and data directory.
 *
 * @param config_file Configuration file, or NULL for PATHS_DEFAULT_CONFIG.
 * @param data_dir    Data directory, or NULL for PATHS_DEFAULT_DATA_DIR.
 * @return 0, or -1 if a path is longer than PATHS_MAX - 1 (nothing changed).
 */
int paths_init(const char *config_file, const char *data_dir);

/**
 * @brief The configuration file in use.
 *
 * @return Path (static storage).
 */
const char *paths_config(void);

/**
 * @brief The data directory in use.
 *
 * @return Path (static storage).
 */
const char *paths_data_dir(void);

/**
 * @brief Builds "\<data dir\>/\<name\>".
 *
 * @param name File or directory name inside the data directory.
 * @param buf  Output buffer.
 * @param len  Size of buf.
 * @return buf.
 */
char *paths_data(const char *name, char *buf, size_t len);

/** @brief Result of paths_parse_args(). */
typedef enum
{
    PATHS_ARGS_RUN     = 0,   /**< Start the application. */
    PATHS_ARGS_VERSION = 1,   /**< --version: print the version and exit 0. */
    PATHS_ARGS_HELP    = 2,   /**< --help: print usage and exit 0. */
    PATHS_ARGS_ERROR   = 3    /**< Bad arguments: print usage and exit 2. */
} paths_args_t;

/**
 * @brief Parses the command line: -c/--config FILE, -d/--data-dir DIR, -V/--version, -h/--help.
 *
 * On PATHS_ARGS_RUN the paths are set with paths_init().
 *
 * @param argc Argument count.
 * @param argv Arguments.
 * @return What to do (paths_args_t).
 */
paths_args_t paths_parse_args(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* PATHS_H */
