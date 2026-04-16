#include "internal/apth_stats.h"

#ifdef APTH_PROFILE_HOOKS

struct apth_hook_stats __apth_stats_read   = {0};
struct apth_hook_stats __apth_stats_write  = {0};
struct apth_hook_stats __apth_stats_open   = {0};
struct apth_hook_stats __apth_stats_close  = {0};
struct apth_hook_stats __apth_stats_socket = {0};
struct apth_hook_stats __apth_stats_other  = {0};
_Atomic(uint64_t) __apth_stats_dedicated_creates = 0;
_Atomic(uint64_t) __apth_stats_mn_creates = 0;

#endif
