#include "hook_libc/hook_lowlevel_io.h"
#include "internal/apth_fd.h"
#include "internal/types.h"
#include <fcntl.h>
#include <unistd.h>

APTH_DEFINE_HOOK(int, pipe, (int pipefd[2]), (pipefd))
{
    {
        apth_t __ded_cur = CUR_APTH;
        if (__ded_cur != NULL && __ded_cur->is_dedicated)
            return apth_func_raw(pipe)(pipefd);
    }
    apth_hook_debug(pipe);

    // Use pipe2 with O_NONBLOCK to avoid 4 fcntl syscalls (2 per fd)
    int result = pipe2(pipefd, O_NONBLOCK);
    if (result < 0)
        return result;

    // Fast register: already O_NONBLOCK
    apth_fd_register_nonblock(pipefd[0]);
    apth_fd_register_nonblock(pipefd[1]);

    return result;
}
