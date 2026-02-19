#include "apth.h"
#include <stdio.h>

#define NULL ((void *)0)

static const char msg[] = "Hello from child\n";

APTH_CONFIG(cfg,
            cfg->workers = 2;)

APTH_MAIN_BEGIN(argc, argv)
{
    write(2, msg, sizeof(msg));
}
APTH_MAIN_END
