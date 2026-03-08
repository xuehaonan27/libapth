#include "apth.h"
#include "common.h"
#include "internal/types.h"

apth_t apth_self(void)
{
    return CUR_APTH;
}
