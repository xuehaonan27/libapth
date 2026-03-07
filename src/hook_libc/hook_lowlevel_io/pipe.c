#include "hook_libc/hook_lowlevel_io.h"
#include "internal/apth_fd.h"

APTH_DEFINE_HOOK(int, pipe, (int pipefd[2]), (pipefd))
{
    apth_hook_debug(pipe);

    // Call the real pipe
    int result = apth_func_raw(pipe)(pipefd);
    if (result < 0)
        return result;

    // Register both file descriptors
    apth_fd_register(pipefd[0]);
    apth_fd_register(pipefd[1]);

    return result;
}