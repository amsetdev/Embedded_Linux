#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *read_file(const char *filename)
{
    FILE *fp = fopen(filename, "r");
    if (!fp)
        return NULL;

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    rewind(fp);

    char *buffer = malloc(size + 1);
    if (!buffer)
    {
        fclose(fp);
        return NULL;
    }

    if (fread(buffer, 1, size, fp) != (size_t)size)
    {
        fclose(fp);
        free(buffer);
        return NULL;
    }

    buffer[size] = '\0';

    fclose(fp);

    return buffer;
}

int json_get_string(const char *json,
                    const char *key,
                    char *value,
                    size_t value_size)
{
    char pattern[64];

    snprintf(pattern,
             sizeof(pattern),
             "\"%s\"",
             key);

    char *p = strstr(json, pattern);

    if (!p)
        return -1;

    p = strchr(p, ':');

    if (!p)
        return -1;

    p++;

    while (*p == ' ' || *p == '\t' || *p == '\n')
        p++;

    if (*p != '"')
        return -1;

    p++;

    char *end = strchr(p, '"');

    if (!end)
        return -1;

    size_t len = end - p;

    if (len >= value_size)
        len = value_size - 1;

    memcpy(value, p, len);

    value[len] = '\0';

    return 0;
}

int json_get_int(const char *json,
                 const char *key,
                 int *value)
{
    char pattern[64];

    snprintf(pattern,
             sizeof(pattern),
             "\"%s\"",
             key);

    char *p = strstr(json, pattern);

    if (!p)
        return -1;

    p = strchr(p, ':');

    if (!p)
        return -1;

    p++;

    while (*p == ' ' || *p == '\t')
        p++;

    *value = atoi(p);

    return 0;
}

int json_get_string_from(const char *object,
                         const char *key,
                         char *value,
                         size_t value_size)
{
    return json_get_string(object,
                           key,
                           value,
                           value_size);
}

int json_get_int_from(const char *object,
                      const char *key,
                      int *value)
{
    return json_get_int(object,
                        key,
                        value);
}