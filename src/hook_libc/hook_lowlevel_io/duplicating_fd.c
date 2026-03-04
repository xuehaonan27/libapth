#include "hook_libc/hook_lowlevel_io.h"

APTH_DEFINE_HOOK(int, dup, (int old), (old))
{
    TODO("dup");
}

APTH_DEFINE_HOOK(int, dup2, (int old, int new), (old, new))
{
    TODO("dup2");
}

APTH_DEFINE_HOOK(int, dup3, (int old, int new, int flags), (old, new, flags))
{
    TODO("dup3");
}