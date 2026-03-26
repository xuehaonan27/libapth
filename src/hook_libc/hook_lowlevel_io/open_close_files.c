#include "hook_libc/hook_lowlevel_io.h"
#include "apth.h"
#include "internal/types.h"
#include "internal/apth_fd.h"
#include <stdbool.h>
#include <stdarg.h>
#include <sys/syscall.h>

APTH_FETCH_LIBCFUNC(open)

static int __variadic_open(const char *pathname, int flags, va_list vargs)
{
    apth_hook_debug(open);

    mode_t mode = 0;
    bool need_mode_arg = (flags & O_CREAT) || (flags & O_TMPFILE);
    if (need_mode_arg)
        mode = va_arg(vargs, mode_t);

    int fd;
    if (need_mode_arg)
        fd = apth_func_raw(open)(pathname, flags, mode);
    else
        fd = apth_func_raw(open)(pathname, flags);

    if (fd < 0)
        return fd;

    // Register this fd in APTH_FD_TABLE
    apth_fd_register(fd);

    return fd;
}

APTH_INTERNAL int apth_func(open)(const char *pathname, int flags, ...)
{
    va_list arg_list;
    va_start(arg_list, flags);
    int retval = __variadic_open(pathname, flags, arg_list);
    va_end(arg_list);
    return retval;
}

APTH_INTERNAL int open(const char *pathname, int flags, ...)
{
    va_list arg_list;
    va_start(arg_list, flags);
    int retval = __variadic_open(pathname, flags, arg_list);
    va_end(arg_list);
    return retval;
}

APTH_FETCH_LIBCFUNC(open64)

static int __variadic_open64(const char *pathname, int flags, va_list vargs)
{
    apth_hook_debug(open64);

    mode_t mode = 0;
    bool need_mode_arg = (flags & O_CREAT) || (flags & O_TMPFILE);
    if (need_mode_arg)
        mode = va_arg(vargs, mode_t);

    int fd;
    if (need_mode_arg)
        fd = apth_func_raw(open64)(pathname, flags, mode);
    else
        fd = apth_func_raw(open64)(pathname, flags);

    if (fd < 0)
        return fd;

    // Register this fd in APTH_FD_TABLE
    apth_fd_register(fd);

    return fd;
}

APTH_INTERNAL int apth_func(open64)(const char *pathname, int flags, ...)
{
    va_list arg_list;
    va_start(arg_list, flags);
    int retval = __variadic_open64(pathname, flags, arg_list);
    va_end(arg_list);
    return retval;
}

APTH_INTERNAL int open64(const char *pathname, int flags, ...)
{
    va_list arg_list;
    va_start(arg_list, flags);
    int retval = __variadic_open64(pathname, flags, arg_list);
    va_end(arg_list);
    return retval;
}

APTH_FETCH_LIBCFUNC(openat)

static int __variadic_openat(int dirfd, const char *pathname, int flags, va_list vargs)
{
    apth_hook_debug(openat);

    mode_t mode = 0;
    bool need_mode_arg = (flags & O_CREAT) || (flags & O_TMPFILE);
    if (need_mode_arg)
        mode = va_arg(vargs, mode_t);

    int fd;
    if (need_mode_arg)
        fd = apth_func_raw(openat)(dirfd, pathname, flags, mode);
    else
        fd = apth_func_raw(openat)(dirfd, pathname, flags);

    if (fd < 0)
        return fd;

    // Register this fd in APTH_FD_TABLE
    apth_fd_register(fd);
    return fd;
}

APTH_INTERNAL int apth_func(openat)(int dirfd, const char *pathname, int flags, ...)
{
    va_list arg_list;
    va_start(arg_list, flags);
    int retval = __variadic_openat(dirfd, pathname, flags, arg_list);
    va_end(arg_list);
    return retval;
}

APTH_API int openat(int dirfd, const char *pathname, int flags, ...)
{
    va_list arg_list;
    va_start(arg_list, flags);
    int retval = __variadic_openat(dirfd, pathname, flags, arg_list);
    va_end(arg_list);
    return retval;
}

APTH_FETCH_LIBCFUNC(openat64)

static int __variadic_openat64(int dirfd, const char *pathname, int flags, va_list vargs)
{
    apth_hook_debug(openat64);

    mode_t mode = 0;
    bool need_mode_arg = (flags & O_CREAT) || (flags & O_TMPFILE);
    if (need_mode_arg)
        mode = va_arg(vargs, mode_t);

    int fd;
    if (need_mode_arg)
        fd = apth_func_raw(openat64)(dirfd, pathname, flags, mode);
    else
        fd = apth_func_raw(openat64)(dirfd, pathname, flags);

    if (fd < 0)
        return fd;

    // Register this fd in APTH_FD_TABLE
    apth_fd_register(fd);
    return fd;
}

APTH_INTERNAL int apth_func(openat64)(int dirfd, const char *pathname, int flags, ...)
{
    va_list arg_list;
    va_start(arg_list, flags);
    int retval = __variadic_openat64(dirfd, pathname, flags, arg_list);
    va_end(arg_list);
    return retval;
}

APTH_API int openat64(int dirfd, const char *pathname, int flags, ...)
{
    va_list arg_list;
    va_start(arg_list, flags);
    int retval = __variadic_openat64(dirfd, pathname, flags, arg_list);
    va_end(arg_list);
    return retval;
}

// APTH_DEFINE_HOOK(
//     int, openat2,
//     (int dirfd, const char *pathname, const struct open_how *how, size_t size),
//     (dirfd, pathname, how, size))

/*
MAYBE_UNUSED APTH_INTERNAL apth_func_pfn_t(openat2) apth_func_raw(openat2) = NULL;
APTH_INTERNAL int __FALLBACK_syscall_openat2(int dirfd, const char *pathname, const struct open_how *how, size_t size)
{
    return syscall(SYS_openat2, dirfd, pathname, &how, sizeof(how));
}

APTH_INTERNAL int apth_func_init(openat2)(void)
{
    assert_msg(apth_func_raw(openat2) == NULL, "sanity");
    apth_func_pfn_t(openat2) func = (apth_func_pfn_t(openat2))dlsym(RTLD_NEXT, stringify(openat2));
    if (func == NULL)
        func = __FALLBACK_syscall_openat2;
    apth_debug("found syscall " stringify(openat2) " = %p", func);
    apth_func_raw(openat2) = func;
    assert_msg(apth_func_raw(openat2) != NULL, "sanity");
    return 0;
}
APTH_API int openat2(int dirfd, const char *pathname, const struct open_how *how, size_t size)
{
    return apth_func(openat2)(dirfd, pathname, how, size);
}
APTH_INTERNAL int apth_func(openat2)(int dirfd, const char *pathname, const struct open_how *how, size_t size)
{
    apth_hook_debug(openat2);

    // Invoke libc socket
    int fd = apth_func_raw(openat2)(dirfd, pathname, how, size);
    if (fd < 0)
        return fd;

    // Register this fd in APTH_FD_TABLE
    apth_fd_register(fd);

    return fd;
}
*/

APTH_DEFINE_HOOK(int, creat, (const char *pathname, mode_t mode), (pathname, mode))
{
    apth_hook_debug(creat);

    // Invoke libc creat
    int fd = apth_func_raw(creat)(pathname, mode);
    if (fd < 0)
        return fd;

    // Register this fd in APTH_FD_TABLE
    apth_fd_register(fd);

    return fd;
}

APTH_DEFINE_HOOK(int, creat64, (const char *pathname, mode_t mode), (pathname, mode))
{
    apth_hook_debug(creat64);

    // Invoke libc creat
    int fd = apth_func_raw(creat64)(pathname, mode);
    if (fd < 0)
        return fd;

    // Register this fd in APTH_FD_TABLE
    apth_fd_register(fd);

    return fd;
}

APTH_DEFINE_HOOK(int, close, (int fd), (fd))
{
    apth_hook_debug(close);

    // Clear entry in `APTH_FD_TABLE`
    apth_fd_unregister(fd);

    // Notify ALL schedulers (including our own) that this fd is being closed.
    // Each scheduler will process the notification in its next event manager
    // iteration, failing all local waiters for this fd and cleaning up epoll
    // registrations. This must happen BEFORE the actual close to avoid fd
    // number reuse races.
    apth_notify_fd_closed(fd);

    // Invoke libc close
    return apth_func_raw(close)(fd);
}

APTH_DEFINE_HOOK(int, close_range,
                 (unsigned int lowfd, unsigned int maxfd, int flags),
                 (lowfd, maxfd, flags))
{
    apth_hook_debug(close_range);

    // Unregister and notify for all fds in range before closing
    struct apth_fd_table_snapshot *snap = apth_fd_snapshot_load();
    for (unsigned int fd = lowfd; fd <= maxfd && fd < (unsigned int)snap->capacity; fd++)
    {
        if (apth_util_fd_valid(fd))
        {
            apth_fd_unregister(fd);
            apth_notify_fd_closed(fd);
        }
    }

    // Call the real close_range
    return apth_func_raw(close_range)(lowfd, maxfd, flags);
}

// TODO: implementation
APTH_DEFINE_HOOK(void, closefrom, (int lowfd), (lowfd))
{
    apth_hook_debug(closefrom);

    // Unregister and notify for all fds from lowfd onwards before closing
    struct apth_fd_table_snapshot *snap2 = apth_fd_snapshot_load();
    for (int fd = lowfd; fd < snap2->capacity; fd++)
    {
        if (apth_util_fd_valid(fd))
        {
            apth_fd_unregister(fd);
            apth_notify_fd_closed(fd);
        }
    }

    // Call the real closefrom
    apth_func_raw(closefrom)(lowfd);
}
