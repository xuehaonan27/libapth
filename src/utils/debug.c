#include "debug.h"

#include "common.h"
#include "apth_errno.h"
#include <stdio.h>

void apth_debug_fn(const char *file, int line, const char *function, const char *message, ...)
{
    // TODO: implement
}

void apth_panic_fn(const char *file, int line, const char *function, const char *message, ...)
{
    // TODO: implement, call apth_debug first, then cleanup and exit
}

void apth_todo_fn(const char *file, int line, const char *function, const char *message, ...) {
    // TODO: implement
}


// void apth_debug(const char *file, int line, int argc, const char *fmt, ...)
// {
//     va_list ap;
//     static char str[1024];
//     size_t n;

//     apth_shield
//     {
//         va_start(ap, fmt);
//         if (file != NULL)
//             snprintf(str, sizeof(str), "%d:%s:%04d: ", (int)getpid(), file, line);
//         else
//             str[0] = '\0';
//         n = strlen(str);
//         vsnprintf(str + n, sizeof(str) - n, fmt, ap);
//         va_end(ap);
//         n = strlen(str);
//         str[n++] = '\n';
//         // TODO: call syscall write to write
//         // pth_sc(write)(STDERR_FILENO, str, n);
//     }
//     return;
// }
