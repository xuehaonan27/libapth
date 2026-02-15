#include "debug.h"

#include "common.h"
#include "apth_errno.h"
#include "apth_string.h"
#include "internal_funcs.h"
#include <stdlib.h>

void apth_debug_fn(const char *file, int line, const char *function, const char *message, ...)
{
    va_list ap;
    static char str[1024];
    size_t n;

    apth_shield
    {
        va_start(ap, message);
        if (file != NULL)
            apth_snprintf(str, sizeof(str), "%d:%s:%04d:%s: ", (int)getpid(), file, line, function);
        else
            str[0] = NUL;
        n = strlen(str);
        apth_vsnprintf(str + n, sizeof(str) - n, message, ap);
        va_end(ap);
        n = strlen(str);
        str[n++] = '\n';
        apth_syscall_raw(write)(STDERR_FILENO, str, n);
    }
    return;
}

NORETURN void apth_panic_fn(const char *file, int line, const char *function, const char *message, ...)
{
    apth_debug_fn(file, line, function, "APTH PANICS: %s", message);
    abort();
}

NORETURN void apth_todo_fn(const char *file, int line, const char *function, const char *message, ...)
{
    apth_debug_fn(file, line, function, "APTH NOT IMPLEMENTED: %s", message);
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