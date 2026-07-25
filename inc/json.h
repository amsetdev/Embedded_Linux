#ifndef JSON_H
#define JSON_H

#include <stddef.h>

/* Read entire file into memory.
 * Returns malloc()'d buffer.
 * Caller must free().
 */
char *read_file(const char *filename);

/* Search key in whole JSON text */
int json_get_string(const char *json,
                    const char *key,
                    char *value,
                    size_t value_size);

int json_get_int(const char *json,
                 const char *key,
                 int *value);

/* Search key starting from a JSON object */
int json_get_string_from(const char *object,
                         const char *key,
                         char *value,
                         size_t value_size);

int json_get_int_from(const char *object,
                      const char *key,
                      int *value);

#endif