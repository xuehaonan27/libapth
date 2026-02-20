#include "apth.h"
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

APTH_CONFIG(cfg,
            cfg->workers = 1;)

static char *child_msgs[5] = {
    "Hi from child apth 0\n",
    "Hi from child apth 1\n",
    "Hi from child apth 2\n",
    "Hi from child apth 3\n",
    "Hi from child apth 4\n",
};

static void *child_apth(void *arg)
{
    int id = (int)arg;

    write(2, child_msgs[id], strlen(child_msgs[id]));

    return (void *)id;
}

APTH_MAIN_BEGIN(argc, argv)
{
    apth_t tids[5];
    void *cdatas[5];

    static char main_hi[] = "Hi from main apth\n";
    write(2, main_hi, sizeof(main_hi));

    for (int i = 0; i < 5; i++)
    {
        apth_attr_t child_attr;
        apth_attr_init(&child_attr);
        if (i == 2)
            apth_attr_setdetachstate(&child_attr, APTH_CREATE_DETACHED);
        apth_create(&tids[i], &child_attr, child_apth, (void *)i);
        apth_attr_destroy(&child_attr);
    }
    sleep(2);

    for (int i = 0; i < 5; i++)
    {
        apth_join(tids[i], &cdatas[i]);
    }

    for (int i = 0; i < 5; i++)
    {
        fprintf(stderr, "child apth %d yields %d\n", i, (int)cdatas[i]);
        if (i == 2)
        {
            if ((int)cdatas[i] != 0 || errno != EINVAL)
            {
                static char err_2_msg[] = "child apth 2 should yield EINVAL, but not\n";
                write(2, err_2_msg, sizeof(err_2_msg));
            }
        }
        else
        {
            if ((int)cdatas[i] != i)
            {
                static char err_other_msg[] = "child apth should yield identity, but not\n";
                write(2, err_other_msg, sizeof(err_other_msg));
            }
        }
    }
}
APTH_MAIN_END