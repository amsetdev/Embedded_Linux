/**
 * @file paths.c
 * @brief Configuration file and data directory (see paths.h).
 */

#include "paths.h"

#include <stdio.h>
#include <string.h>

/** @brief Configuration file in use. */
static char config_path[PATHS_MAX] = PATHS_DEFAULT_CONFIG;

/** @brief Data directory in use. */
static char data_dir[PATHS_MAX] = PATHS_DEFAULT_DATA_DIR;

/* Documented in paths.h. */
int paths_init(const char *config_file, const char *dir)
{
    if (!config_file)
        config_file = PATHS_DEFAULT_CONFIG;
    if (!dir)
        dir = PATHS_DEFAULT_DATA_DIR;

    if (strlen(config_file) >= sizeof(config_path) || strlen(dir) >= sizeof(data_dir)
        || config_file[0] == '\0' || dir[0] == '\0')
        return -1;

    snprintf(config_path, sizeof(config_path), "%s", config_file);
    snprintf(data_dir, sizeof(data_dir), "%s", dir);

    /* "/var/lib/gateway/" and "/var/lib/gateway" are the same directory. */
    size_t n = strlen(data_dir);
    while (n > 1 && data_dir[n - 1] == '/')
        data_dir[--n] = '\0';

    return 0;
}

/* Documented in paths.h. */
const char *paths_config(void)
{
    return config_path;
}

/* Documented in paths.h. */
const char *paths_data_dir(void)
{
    return data_dir;
}

/* Documented in paths.h. */
char *paths_data(const char *name, char *buf, size_t len)
{
    if (strcmp(data_dir, "/") == 0)
        snprintf(buf, len, "/%s", name);
    else
        snprintf(buf, len, "%s/%s", data_dir, name);
    return buf;
}

/* Documented in paths.h. */
paths_args_t paths_parse_args(int argc, char **argv)
{
    const char *config_file = NULL;
    const char *dir = NULL;

    for (int i = 1; i < argc; i++)
    {
        const char *a = argv[i];

        if (strcmp(a, "-V") == 0 || strcmp(a, "--version") == 0)
            return PATHS_ARGS_VERSION;
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0)
            return PATHS_ARGS_HELP;

        if (strcmp(a, "-c") == 0 || strcmp(a, "--config") == 0)
        {
            if (++i >= argc)
                return PATHS_ARGS_ERROR;
            config_file = argv[i];
        }
        else if (strncmp(a, "--config=", 9) == 0)
        {
            config_file = a + 9;
        }
        else if (strcmp(a, "-d") == 0 || strcmp(a, "--data-dir") == 0)
        {
            if (++i >= argc)
                return PATHS_ARGS_ERROR;
            dir = argv[i];
        }
        else if (strncmp(a, "--data-dir=", 11) == 0)
        {
            dir = a + 11;
        }
        else
        {
            return PATHS_ARGS_ERROR;
        }
    }

    return paths_init(config_file, dir) == 0 ? PATHS_ARGS_RUN : PATHS_ARGS_ERROR;
}
