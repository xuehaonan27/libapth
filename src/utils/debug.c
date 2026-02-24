#include "debug.h"

#include "common.h"
#include "apth_errno.h"
#include "apth_string.h"
#include "atomic_wrapper.h"
#include "internal_funcs.h"
#include <stdlib.h>
#include <stdio.h>

static _Atomic(int) DEBUG_USING_HOOKED = 0;

APTH_INTERNAL int get_DEBUG_USING_HOOKED(void)
{
    return atomic_load_acquire(&DEBUG_USING_HOOKED);
}

APTH_INTERNAL void set_DEBUG_USING_HOOKED(int v)
{
    atomic_store_release(&DEBUG_USING_HOOKED, v);
}

static char str[1024];

_Atomic int dbg_spinl = 0;
static void _dbg_spin_lock(void)
{
    int expected = 0;
    while (!atomic_compare_exchange_weak_acquire(&dbg_spinl, &expected, 1))
    {
        expected = 0;
    }
}

static void _dbg_spin_unlock(void)
{
    atomic_store_release(&dbg_spinl, 0);
}

void apth_debug_fn(const char *file, int line, const char *function, const char *message, ...)
{
    if (get_DEBUG_USING_HOOKED() == 0)
    {
        va_list ap;
        size_t n;

        va_start(ap, message);
        _dbg_spin_lock();

        if (file != NULL)
            snprintf(str, sizeof(str), "%s:%04d:%s: (nil) ", file, line, function);
        else
            str[0] = NUL;
        n = strlen(str);
        vsnprintf(str + n, sizeof(str) - n, message, ap);
        va_end(ap);
        n = strlen(str);
        str[n++] = '\n';
        str[n++] = '\0';
        fwrite(str, 1, n, stderr);

        _dbg_spin_unlock();
        return;
    }

    va_list ap;
    size_t n;

    apth_shield
    {
        _dbg_spin_lock();

        va_start(ap, message);
        if (file != NULL)
            apth_snprintf(str, sizeof(str), "%s:%04d:%s: (%d) ", file, line, function, cur_sched()->id);
        else
            str[0] = NUL;
        n = strlen(str);
        apth_vsnprintf(str + n, sizeof(str) - n, message, ap);
        va_end(ap);
        n = strlen(str);
        str[n++] = '\n';
        str[n++] = '\0';
        apth_syscall_raw(write)(STDERR_FILENO, str, n);

        _dbg_spin_unlock();
    }
    return;
}

static void apth_vdebug_fn(const char *file, int line, const char *function,
                           const char *msg1, const char *msg2, va_list args)
{
    if (get_DEBUG_USING_HOOKED() == 0)
    {
        size_t n;

        _dbg_spin_lock();

        if (file != NULL)
            snprintf(str, sizeof(str), "%s:%04d:%s: (nil) ", file, line, function);
        else
            str[0] = '\0';
        n = strlen(str);
        vsnprintf(str + n, sizeof(str) - n, msg1, args);
        n = strlen(str);
        vsnprintf(str + n, sizeof(str) - n, msg2, args);
        n = strlen(str);
        str[n++] = '\n';
        str[n++] = '\0';
        fwrite(str, 1, n, stderr);

        _dbg_spin_unlock();
        return;
    }

    size_t n;

    apth_shield
    {
        _dbg_spin_lock();

        if (file != NULL)
            apth_snprintf(str, sizeof(str), "%s:%04d:%s: (%d) ", file, line, function, cur_sched()->id);
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