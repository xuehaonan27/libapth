#include "hook_libc/hook_lowlevel_io.h"
// #include "internal_types.h"
// #include "internal_funcs.h"

APTH_DEFINE_HOOK(ssize_t, copy_file_range,
                 (int inputfd, off64_t *inputpos, int outputfd, off64_t *outputpos,
                  ssize_t length, unsigned int flags /* must be zero */),
                 (inputfd, inputpos, outputfd, outputpos, length, flags))
{
    TODO("copy_file_range");
}
