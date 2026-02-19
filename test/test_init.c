// Test initializing the LIBAPTH package

#include "apth.h"
#include <stdio.h>

#define NULL ((void *)0)

static char a[] = "Hello\n";

APTH_CONFIG(cfg,
            cfg->workers = 1;)

APTH_MAIN_BEGIN(argc, argv)
{
    write(2, a, sizeof(a));
}
APTH_MAIN_END
