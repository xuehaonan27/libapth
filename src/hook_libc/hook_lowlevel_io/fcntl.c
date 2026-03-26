#include "hook_libc/hook_lowlevel_io.h"
#include "internal/apth_fd.h"
#include "internal/types.h"
#include "utils/apth_errno.h"
#include "utils/atomic_wrapper.h"
#include <stdarg.h>
#include <fcntl.h>
#include <sys/types.h>

APTH_FETCH_LIBCFUNC(fcntl)

APTH_INTERNAL int apth_func(fcntl)(int fd, int cmd, ...)
{
    {
        apth_t __ded_cur = CUR_APTH;
        if (__ded_cur != NULL && __ded_cur->is_dedicated)
        {
            va_list args;
            va_start(args, cmd);
            int result;
            switch (cmd)
            {
            case F_SETFL:
            case F_DUPFD:
            case F_DUPFD_CLOEXEC:
            case F_SETFD:
            case F_SETOWN:
            case F_SETSIG:
            case F_SETLEASE:
            case F_NOTIFY:
            case F_SETPIPE_SZ:
#ifdef F_ADD_SEALS
            case F_ADD_SEALS:
#endif
            {
                int arg = va_arg(args, int);
                result = apth_func_raw(fcntl)(fd, cmd, arg);
                break;
            }
            case F_SETLK:
            case F_SETLKW:
            case F_GETLK:
#ifdef F_OFD_SETLK
            case F_OFD_SETLK:
            case F_OFD_SETLKW:
            case F_OFD_GETLK:
#endif
            {
                struct flock *lock = va_arg(args, struct flock *);
                result = apth_func_raw(fcntl)(fd, cmd, lock);
                break;
            }
#ifdef F_GETOWN_EX
            case F_GETOWN_EX:
            case F_SETOWN_EX:
            {
                struct f_owner_ex *owner = va_arg(args, struct f_owner_ex *);
                result = apth_func_raw(fcntl)(fd, cmd, owner);
                break;
            }
#endif
            default:
                result = apth_func_raw(fcntl)(fd, cmd);
                break;
            }
            va_end(args);
            return result;
        }
    }
    apth_hook_debug(fcntl);

    va_list args;
    va_start(args, cmd);

    // Special handling for F_SETFL to preserve O_NONBLOCK on managed FDs
    if (cmd == F_SETFL)
    {
        int flags = va_arg(args, int);

        // If this FD is managed by LIBAPTH, force O_NONBLOCK
        if (apth_fd_is_managed(fd))
        {
            // Silently add O_NONBLOCK to whatever flags user requested
            flags |= O_NONBLOCK;
            apth_debug("fcntl F_SETFL on managed fd=%d, forcing O_NONBLOCK", fd);
        }

        va_end(args);
        return apth_func_raw(fcntl)(fd, cmd, flags);
    }

    // For F_DUPFD and F_DUPFD_CLOEXEC, we need to register the new FD
    if (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC)
    {
        int arg = va_arg(args, int);
        int new_fd = apth_func_raw(fcntl)(fd, cmd, arg);
        va_end(args);

        if (new_fd >= 0 && apth_fd_is_managed(fd))
        {
            // Original FD was managed, so manage the duplicate too
            apth_fd_register(new_fd);
        }
        return new_fd;
    }

    // Handle other fcntl commands based on their argument types
    int result;
    switch (cmd)
    {
    // Commands taking int argument
    case F_SETFD:
    case F_SETOWN:
    case F_SETSIG:
    case F_SETLEASE:
    case F_NOTIFY:
    case F_SETPIPE_SZ:
#ifdef F_ADD_SEALS
    case F_ADD_SEALS:
#endif
    {
        int arg = va_arg(args, int);
        result = apth_func_raw(fcntl)(fd, cmd, arg);
        break;
    }

    // Commands taking struct flock* argument
    case F_SETLK:
    case F_SETLKW:
    case F_GETLK:
#ifdef F_OFD_SETLK
    case F_OFD_SETLK:
    case F_OFD_SETLKW:
    case F_OFD_GETLK:
#endif
    {
        struct flock *lock = va_arg(args, struct flock *);
        result = apth_func_raw(fcntl)(fd, cmd, lock);
        break;
    }

    // Commands taking struct f_owner_ex* argument
#ifdef F_GETOWN_EX
    case F_GETOWN_EX:
    case F_SETOWN_EX:
    {
        struct f_owner_ex *owner = va_arg(args, struct f_owner_ex *);
        result = apth_func_raw(fcntl)(fd, cmd, owner);
        break;
    }
#endif

    // Commands taking no argument (F_GETFL, F_GETFD, F_GETOWN, etc.)
    default:
        result = apth_func_raw(fcntl)(fd, cmd);
        break;
    }

    va_end(args);
    return result;
}

// Export the symbol
APTH_API int fcntl(int fd, int cmd, ...)
{
    {
        apth_t __ded_cur = CUR_APTH;
        if (__ded_cur != NULL && __ded_cur->is_dedicated)
        {
            va_list args;
            va_start(args, cmd);
            int result;
            switch (cmd)
            {
            case F_SETFL:
            case F_DUPFD:
            case F_DUPFD_CLOEXEC:
            case F_SETFD:
            case F_SETOWN:
            case F_SETSIG:
            case F_SETLEASE:
            case F_NOTIFY:
            case F_SETPIPE_SZ:
#ifdef F_ADD_SEALS
            case F_ADD_SEALS:
#endif
            {
                int arg = va_arg(args, int);
                result = apth_func_raw(fcntl)(fd, cmd, arg);
                break;
            }
            case F_SETLK:
            case F_SETLKW:
            case F_GETLK:
#ifdef F_OFD_SETLK
            case F_OFD_SETLK:
            case F_OFD_SETLKW:
            case F_OFD_GETLK:
#endif
            {
                struct flock *lock = va_arg(args, struct flock *);
                result = apth_func_raw(fcntl)(fd, cmd, lock);
                break;
            }
#ifdef F_GETOWN_EX
            case F_GETOWN_EX:
            case F_SETOWN_EX:
            {
                struct f_owner_ex *owner = va_arg(args, struct f_owner_ex *);
                result = apth_func_raw(fcntl)(fd, cmd, owner);
                break;
            }
#endif
            default:
                result = apth_func_raw(fcntl)(fd, cmd);
                break;
            }
            va_end(args);
            return result;
        }
    }
    va_list args;
    va_start(args, cmd);

    // Forward to our hooked version
    // We need to handle the variadic args carefully
    int result;
    switch (cmd)
    {
    case F_SETFL:
    case F_DUPFD:
    case F_DUPFD_CLOEXEC:
    case F_SETFD:
    case F_SETOWN:
    case F_SETSIG:
    case F_SETLEASE:
    case F_NOTIFY:
    case F_SETPIPE_SZ:
#ifdef F_ADD_SEALS
    case F_ADD_SEALS:
#endif
    {
        int arg = va_arg(args, int);
        result = apth_func(fcntl)(fd, cmd, arg);
        break;
    }
    case F_SETLK:
    case F_SETLKW:
    case F_GETLK:
#ifdef F_OFD_SETLK
    case F_OFD_SETLK:
    case F_OFD_SETLKW:
    case F_OFD_GETLK:
#endif
    {
        struct flock *lock = va_arg(args, struct flock *);
        result = apth_func(fcntl)(fd, cmd, lock);
        break;
    }
#ifdef F_GETOWN_EX
    case F_GETOWN_EX:
    case F_SETOWN_EX:
    {
        struct f_owner_ex *owner = va_arg(args, struct f_owner_ex *);
        result = apth_func(fcntl)(fd, cmd, owner);
        break;
    }
#endif
    default:
        result = apth_func(fcntl)(fd, cmd);
        break;
    }

    va_end(args);
    return result;
}