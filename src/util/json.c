#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Reads the contents of a file into a dynamically allocated buffer.
 *
 * Opens the specified file, reads its entire contents, and returns
 * a null-terminated string containing the file data.
 *
 * @param filename Path to the file to read.
 *
 * @return Pointer to a dynamically allocated buffer containing the file
 *         contents on success, or NULL on failure.
 *
 * @note The caller is responsible for freeing the returned buffer using free().
 */
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

/**
 * @brief Retrieves a string value associated with a key from a JSON string.
 *
 * Searches for the specified key in the JSON text and copies its
 * string value into the provided buffer.
 *
 * @param json Pointer to the JSON string.
 * @param key JSON key to search for.
 * @param value Buffer to store the extracted string.
 * @param value_size Size of the destination buffer in bytes.
 *
 * @return 0 on success, -1 if the key or value is not found or invalid.
 */
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

/**
 * @brief Retrieves an integer value associated with a key from a JSON string.
 *
 * Searches for the specified key in the JSON text and converts the
 * associated value to an integer.
 *
 * @param json Pointer to the JSON string.
 * @param key JSON key to search for.
 * @param value Pointer to store the extracted integer.
 *
 * @return 0 on success, -1 if the key is not found or invalid.
 */
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

/**
 * @brief Retrieves a string value from a JSON object.
 *
 * Wrapper around json_get_string() for extracting a string value
 * from a specific JSON object.
 *
 * @param object Pointer to the JSON object.
 * @param key JSON key to search for.
 * @param value Buffer to store the extracted string.
 * @param value_size Size of the destination buffer in bytes.
 *
 * @return 0 on success, -1 on failure.
 */
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

/**
 * @brief Retrieves an integer value from a JSON object.
 *
 * Wrapper around json_get_int() for extracting an integer value
 * from a specific JSON object.
 *
 * @param object Pointer to the JSON object.
 * @param key JSON key to search for.
 * @param value Pointer to store the extracted integer.
 *
 * @return 0 on success, -1 on failure.
 */
int json_get_int_from(const char *object,
                      const char *key,
                      int *value)
{
    return json_get_int(object,
                        key,
                        value);
}