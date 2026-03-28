#include "hook_libc/hook_lowlevel_io.h"
#include "apth.h"
#include "internal/types.h"
#include "internal/apth_fd.h"

APTH_DEFINE_HOOK(int, dup, (int old), (old))
{
    {
        apth_t __ded_cur = CUR_APTH;
        if (__ded_cur == NULL || __ded_cur->is_dedicated)
            return apth_func_raw(dup)(old);
    }
    apth_hook_debug(dup);

    // Call the real dup
    int newfd = apth_func_raw(dup)(old);
    if (newfd < 0)
        return newfd;

    // Register the new fd
    apth_fd_register(newfd);

    return newfd;
}

APTH_DEFINE_HOOK(int, dup2, (int old, int new), (old, new))
{
    {
        apth_t __ded_cur = CUR_APTH;
        if (__ded_cur == NULL || __ded_cur->is_dedicated)
            return apth_func_raw(dup2)(old, new);
    }
    apth_hook_debug(dup2);

    // If new fd is already open, dup2 will close it first
    // We need to unregister it before the actual dup2 call
    if (old != new && apth_util_fd_valid(new))
    {
        apth_fd_unregister(new);
        apth_notify_fd_closed(new);
    }

    // Call the real dup2
    int result = apth_func_raw(dup2)(old, new);
    if (result < 0)
        return result;

    // Register the new fd
    apth_fd_register(result);

    return result;
}

APTH_DEFINE_HOOK(int, dup3, (int old, int new, int flags), (old, new, flags))
{
    {
        apth_t __ded_cur = CUR_APTH;
        if (__ded_cur == NULL || __ded_cur->is_dedicated)
            return apth_func_raw(dup3)(old, new, flags);
    }
    apth_hook_debug(dup3);

    // If new fd is already open, dup3 will close it first
    // We need to unregister it before the actual dup3 call
    if (old != new && apth_util_fd_valid(new))
    {
        apth_fd_unregister(new);
        apth_notify_fd_closed(new);
    }

    // Call the real dup3
    int result = apth_func_raw(dup3)(old, new, flags);
    if (result < 0)
        return result;

    // Register the new fd
    apth_fd_register(result);

    return result;
}