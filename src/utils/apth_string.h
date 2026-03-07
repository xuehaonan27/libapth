#ifndef __LIBAPTH_UTILS_APTH_STRING_H
#define __LIBAPTH_UTILS_APTH_STRING_H

#include <sys/types.h>
#include <stdarg.h>

#define NUL '\0'

int apth_vsnprintf(char *str, ssize_t count, const char *fmt, va_list args);
int apth_snprintf(char *str, ssize_t count, const char *fmt, ...);
char *apth_vasprintf(const char *fmt, va_list ap);
char *apth_asprintf(const char *fmt, ...);

#endif // __LIBAPTH_UTILS_APTH_STRING_H
