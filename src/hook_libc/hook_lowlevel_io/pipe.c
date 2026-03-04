#include "hook_libc/hook_lowlevel_io.h"
#include "internal_types.h"
#include "internal_funcs.h"

APTH_DEFINE_HOOK(int, pipe, (int pipefd[2]), (pipefd))
{
    TODO("pipe");
}