#ifndef __LIBATH_UTILS_DEBUG_H
#define __LIBATH_UTILS_DEBUG_H

#include "archplattoold.h"

APTH_INTERNAL int get_DEBUG_USING_HOOKED(void);
APTH_INTERNAL void set_DEBUG_USING_HOOKED(int b);

#ifndef APTH_DEBUG
#define apth_debug(...)
#else
#define apth_debug(...) apth_debug_fn(__FILE__, __LINE__, __func__, __VA_ARGS__)
#endif // APTH_DEBUG

void apth_debug_fn(const char *file, int line, const char *function, const char *message, ...);
void apth_panic_fn(const char *file, int line, const char *function, const char *message, ...);
void apth_todo_fn(const char *file, int line, const char *function, const char *message, ...);

#define PANIC(...) apth_panic_fn(__FILE__, __LINE__, __func__, __VA_ARGS__)
#define TODO(...) apth_todo_fn(__FILE__, __LINE__, __func__, __VA_ARGS__)

#define assert(p)                           \
    do                                      \
    {                                       \
        if (!(p))                           \
            PANIC("assert(" #p ") failed"); \
    } while (0)

#define assert_msg(p, ...)                                \
    do                                                    \
    {                                                     \
        if (!(p))                                         \
            PANIC("assert(" #p ") failed, " __VA_ARGS__); \
    } while (0)

#endif // __LIBATH_UTILS_DEBUG_H
