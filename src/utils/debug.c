#include "debug.h"

#include "common.h"
#include "apth_errno.h"
#include "apth_string.h"
#include "atomic_wrapper.h"
#include "internal_funcs.h"
#include <stdlib.h>

static char str[1024];
// static lll_t buf_lock;

_Atomic int dbg_spinl = 0;
static void _dbg_spin_lock(void) {
    int expected = 0;
    while (!atomic_compare_exchange_weak_acquire(&dbg_spinl, &expected, 1)) {
        expected = 0;
    }
}

static void _dbg_spin_unlock(void) {
    atomic_store_release(&dbg_spinl, 0);
}

void apth_debug_fn(const char *file, int line, const char *function, const char *message, ...)
{
    va_list ap;
    size_t n;

    apth_shield
    {
        pthread_t tid = apth_syscall_raw(pthread_self)();
// #ifdef APTH_DEBUG_LLL
//         char lll_str[256];
//         apth_snprintf(lll_str, sizeof(lll_str), "%p:%s:%04d:%s: debug enter", tid, file, line, function);
//         lll_lock(&buf_lock, lll_str);
// #else  // APTH_DEBUG_LLL
//         lll_lock(&buf_lock, "dummy");
// #endif // APTH_DEBUG_LLL

        _dbg_spin_lock();

        va_start(ap, message);
        if (file != NULL)
            apth_snprintf(str, sizeof(str), "%p:%s:%04d:%s: ", tid, file, line, function);
        else
            str[0] = NUL;
        n = strlen(str);
        apth_vsnprintf(str + n, sizeof(str) - n, message, ap);
        va_end(ap);
        n = strlen(str);
        str[n++] = '\n';
        str[n++] = '\0';
        apth_syscall_raw(write)(STDERR_FILENO, str, n);
// #ifdef APTH_DEBUG_LLL
//         apth_snprintf(lll_str, sizeof(lll_str), "%p:%s:%04d:%s: debug leave", tid, file, line, function);
//         lll_unlock(&buf_lock, lll_str);
// #else  // APTH_DEBUG_LLL
//         lll_unlock(&buf_lock, "dummy");
// #endif // APTH_DEBUG_LLL

        _dbg_spin_unlock();

    }
    return;
}

static void apth_vdebug_fn(const char *file, int line, const char *function,
                           const char *msg1, const char *msg2, va_list args)
{
    size_t n;

    apth_shield
    {
        pthread_t tid = apth_syscall_raw(pthread_self)();
        // lll_lock(&buf_lock, "vdebug enter");
        _dbg_spin_lock();
        if (file != NULL)
            apth_snprintf(str, sizeof(str), "%p:%s:%04d:%s: ", tid, file, line, function);
        else
            str[0] = '\0';
        n = strlen(str);
        apth_vsnprintf(str + n, sizeof(str) - n, msg1, args);
        n = strlen(str);
        apth_vsnprintf(str + n, sizeof(str) - n, msg2, args);
        n = strlen(str);
        str[n++] = '\n';
        str[n++] = '\0';
        apth_syscall_raw(write)(STDERR_FILENO, str, n);
        // lll_unlock(&buf_lock, "vdebug leave");
        _dbg_spin_unlock();
    }
    return;
}

NORETURN void apth_panic_fn(const char *file, int line, const char *function, const char *message, ...)
{
    va_list args;
    va_start(args, message);
    apth_vdebug_fn(file, line, function, "APTH PANICS: ", message, args);
    va_end(args);
    abort();
}

NORETURN void apth_todo_fn(const char *file, int line, const char *function, const char *message, ...)
{
    va_list args;
    va_start(args, message);
    apth_vdebug_fn(file, line, function, "APTH NOT IMPLEMENTED: %s", message, args);
    va_end(args);
    abort();
}

void apth_dump_thread_list(FILE *fp, const char *qn, struct list *l)
{
    apth_t t;
    int i;

    fprintf(fp, "| Thread Queue %s:\n", qn);
    if (list_empty(l))
        fprintf(fp, "|   no threads\n");
    i = 1;
    FOR_ELEMENT_IN_LIST_REF(l, e)
    {
        t = apth_t_list_entry(e);
        fprintf(fp, "|   %d. thread 0x%lx (\"%s\")\n", i++, (unsigned long)t, t->name);
    }
}