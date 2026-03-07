#include "apth.h"
#include "common.h"
#include "internal/apth_sched.h"

apth_t apth_self(void)
{
    return CUR_APTH;
}
