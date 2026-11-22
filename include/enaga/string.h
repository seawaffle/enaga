/* Enaga: the C string and memory functions the app runtime provides (runtime/runtime.cpp), for C and C++. Apps
 * get no C library, so there is no <string.h>; include this one instead. The rest of <string.h> (strcpy, strcat,
 * strstr, ...) is not provided: <enaga/ui.hpp>'s Line builds text, and a missing function shows as an undefined
 * reference when the app links. */
#ifndef ENAGA_STRING_H
#define ENAGA_STRING_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *memcpy(void *destination, const void *source, size_t bytes);
void *memmove(void *destination, const void *source, size_t bytes);
void *memset(void *destination, int value, size_t bytes);
int memcmp(const void *a, const void *b, size_t bytes);
size_t strlen(const char *text);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t bytes);
char *strchr(const char *text, int c);         /* the first c in text (c may be 0: the end), or NULL */
char *strrchr(const char *text, int c);        /* the last */

#ifdef __cplusplus
}
#endif

#endif
