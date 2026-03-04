#include "hook_pthread.h"
#include "internal_types.h"
#include "internal_funcs.h"

#define X APTH_FETCH_LIBCFUNC
APTH_LIST_OF_HOOK_PTHREAD
#undef X
