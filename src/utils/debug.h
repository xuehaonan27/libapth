#ifndef __LIBATH_UTILS_DEBUG_H
#define __LIBATH_UTILS_DEBUG_H

// #ifndef APTH_DEBUG

// #define apth_debug1(a1)                     /* NOP */
// #define apth_debug2(a1, a2)                 /* NOP */
// #define apth_debug3(a1, a2, a3)             /* NOP */
// #define apth_debug4(a1, a2, a3, a4)         /* NOP */
// #define apth_debug5(a1, a2, a3, a4, a5)     /* NOP */
// #define apth_debug6(a1, a2, a3, a4, a5, a6) /* NOP */

// #else

// #define apth_debug1(a1) apth_debug(__FILE__, __LINE__, 1, a1)
// #define apth_debug2(a1, a2) apth_debug(__FILE__, __LINE__, 2, a1, a2)
// #define apth_debug3(a1, a2, a3) apth_debug(__FILE__, __LINE__, 3, a1, a2, a3)
// #define apth_debug4(a1, a2, a3, a4) apth_debug(__FILE__, __LINE__, 4, a1, a2, a3, a4)
// #define apth_debug5(a1, a2, a3, a4, a5) apth_debug(__FILE__, __LINE__, 5, a1, a2, a3, a4, a5)
// #define apth_debug6(a1, a2, a3, a4, a5, a6) apth_debug(__FILE__, __LINE__, 6, a1, a2, a3, a4, a5, a6)

// #endif // APTH_DEBUG

#ifndef APTH_DEBUG
#define apth_debug(...)
#else
#define apth_debug(...) apth_debug_fn(__FILE__, __LINE__, __func__, __VA_ARGS__)
#endif // APTH_DEBUG

void apth_debug_fn(const char *file, int line, const char *function, const char *message, ...);
void apth_panic_fn(const char *file, int line, const char *function, const char *message, ...);
void apth_todo_fn(const char *file, int line, const char *function, const char *message, ...);

// GCC attributes
// #define UNUSED
// #define NO_RETURN __attribute__((noreturn))
// #define NO_INLINE __attribute__((noinline))

#define PANIC(...) apth_panic_fn(__FILE__, __LINE__, __func__, __VA_ARGS__)
#define TODO(...) apth_todo_fn(__FILE__, __LINE__, __func__, __VA_ARGS__)

#define assert(p)                           \
    do                                      \
    {                                       \
        if (!(p))                           \
            PANIC("assert(" #p ") failed"); \
    } while (0)

#define assert_msg(p, ...)                               \
    do                                                   \
    {                                                    \
        if (!(p))                                        \
            PANIC("assert(" #p ") failed, " __VA_ARGS__); \
    } while (0)

#endif // __LIBATH_UTILS_DEBUG_H
