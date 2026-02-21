#include "apth.h"
#include "internal_funcs.h"
#include "internal_types.h"
#include "utils/atomic_wrapper.h"
#include "utils/archplattoold.h"

void apth_testcancel(void)
{
    apth_cancel_point();
}