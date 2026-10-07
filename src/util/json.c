#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
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
 * @brief Finds the closing quote of a JSON string.
 *
 * @param p First character after the opening quote.
 * @return Pointer to the closing quote, or NULL if the string is unterminated.
 */
static const char *string_end(const char *p)
{
    while (*p && *p != '"')
    {
        if (*p == '\\' && p[1] != '\0')
            p++;
        p++;
    }
    return *p == '"' ? p : NULL;
}

/**
 * @brief Value of one hex digit.
 *
 * @param c Character.
 * @return 0-15, or -1 if c is not a hex digit.
 */
static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/**
 * @brief Skips JSON whitespace.
 *
 * @param p Position in the JSON text.
 * @return First non-whitespace character at or after p.
 */
static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    return p;
}

/*
 * @brief Retrieves a string value associated with a key from a JSON string.
 *
 * Searches for the specified key in the JSON text and copies its
 * string value, with escapes decoded, into the provided buffer.
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

    const char *p = strstr(json, pattern);

    if (!p)
        return -1;

    p = strchr(p, ':');

    if (!p)
        return -1;

    p = skip_ws(p + 1);

    if (*p != '"')
        return -1;

    p++;

    const char *end = string_end(p);

    if (!end || value_size == 0)
        return -1;

    /* Decode escapes; truncate to value_size - 1 bytes. */
    size_t len = 0;

    while (p < end)
    {
        char c = *p++;

        if (c == '\\')
        {
            char e = *p++;

            switch (e)
            {
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'u':
            {
                int cp = 0;
                int ok = 1;
                for (int i = 0; i < 4; i++)
                {
                    int d = (p + i < end) ? hex_digit(p[i]) : -1;
                    if (d < 0)
                    {
                        ok = 0;
                        break;
                    }
                    cp = cp * 16 + d;
                }
                if (ok)
                    p += 4;
                /* ASCII is kept; other code points become '?' (no UTF-8 encoding). */
                c = (ok && cp > 0 && cp < 0x80) ? (char)cp : '?';
                break;
            }
            default:            /* \" \\ \/ and unknown escapes: the character itself */
                c = e;
                break;
            }
        }

        if (len + 1 < value_size)
            value[len++] = c;
    }

    value[len] = '\0';

    return 0;
}

/*
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

/*
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

/*
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

/* Documented in json.h. */
const char *json_value_end(const char *p)
{
    if (!p || (*p != '{' && *p != '['))
        return NULL;

    int depth = 0;

    for (; *p; p++)
    {
        if (*p == '"')
        {
            p = string_end(p + 1);
            if (!p)
                return NULL;
        }
        else if (*p == '{' || *p == '[')
        {
            depth++;
        }
        else if (*p == '}' || *p == ']')
        {
            if (--depth == 0)
                return p;
        }
    }

    return NULL;
}

/* Documented in json.h. */
char *json_object_dup(const char *json, const char *key)
{
    char pattern[64];

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    for (const char *p = strstr(json, pattern); p; p = strstr(p + 1, pattern))
    {
        const char *q = skip_ws(p + strlen(pattern));

        if (*q != ':')
            continue;           /* the name appears as a value, not as a key */

        q = skip_ws(q + 1);

        if (*q != '{')
            continue;

        const char *end = json_value_end(q);

        if (!end)
            return NULL;

        size_t n = (size_t)(end - q) + 1;
        char *copy = malloc(n + 1);

        if (!copy)
            return NULL;

        memcpy(copy, q, n);
        copy[n] = '\0';
        return copy;
    }

    return NULL;
}
