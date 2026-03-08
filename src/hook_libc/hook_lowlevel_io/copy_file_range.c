#include "hook_libc/hook_lowlevel_io.h"
#include "apth.h"
#include "internal/types.h"
#include "internal/apth_event.h"
#include "internal/apth_fd.h"

APTH_DEFINE_HOOK(ssize_t, copy_file_range,
                 (int inputfd, off64_t *inputpos, int outputfd, off64_t *outputpos,
                  size_t length, unsigned int flags /* must be zero */),
                 (inputfd, inputpos, outputfd, outputpos, length, flags))
{
    apth_hook_debug(copy_file_range);

    apth_t cur = CUR_APTH;
    apth_debug("apth_func_copy_file_range: enter from thread \"%s\"", cur->name);

    // POSIX compliance
    if (length == 0)
        return 0;
    if (!apth_util_fd_valid(inputfd))
        return apth_error(-1, EBADF);
    if (!apth_util_fd_valid(outputfd))
        return apth_error(-1, EBADF);

    // Acquire both file descriptors
    int input_orig_mode = apth_fd_acquire(inputfd);
    if (input_orig_mode < 0)
        return apth_error(-1, EBADF);

    int output_orig_mode = apth_fd_acquire(outputfd);
    if (output_orig_mode < 0)
    {
        apth_fd_release(inputfd);
        return apth_error(-1, EBADF);
    }

    ssize_t rv;
    for (;;)
    {
        while ((rv = apth_func_raw(copy_file_range)(inputfd, inputpos, outputfd, outputpos, length, flags)) < 0 && errno == EINTR)
            ;

        if (rv < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            // Wait for both input readable and output writable
            apth_event_t ev_in = apth_event_fd(APTH_GOAL_UNTIL_FD_READABLE | APTH_EVENT_MODE_STATIC, inputfd);
            apth_event_t ev_out = apth_event_fd(APTH_GOAL_UNTIL_FD_WRITEABLE | APTH_EVENT_MODE_STATIC, outputfd);
            apth_wait_event(ev_in);
            apth_wait_event(ev_out);
            apth_event_free(ev_in);
            apth_event_free(ev_out);
            continue;
        }

        // rv >= 0 (success) or rv < 0 (real error)
        break;
    }

    // Restore file descriptor modes
    apth_fd_release(outputfd);
    apth_fd_release(inputfd);

    apth_debug("apth_func_copy_file_range: leave to thread \"%s\"", cur->name);
    return rv;
}
