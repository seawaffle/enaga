/* Minimal <string.h> for the freestanding firmware (no libc). Implementations in src/libc_min.c.
 * GCC may also emit calls to memcpy/memset for struct copies and large initialisers. */
#ifndef STRING_H
#define STRING_H
typedef unsigned int size_t;
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int value, size_t n);
int memcmp(const void *a, const void *b, size_t n);
void *memchr(const void *s, int c, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strchr(const char *s, int c);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t n);
#endif
