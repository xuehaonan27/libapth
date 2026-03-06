#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif // _GNU_SOURCE
#include <sched.h>

#include "apth.h"
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define handle_error_en(en, msg) \
    do                           \
    {                            \
        errno = en;              \
        perror(msg);             \
        exit(EXIT_FAILURE);      \
    } while (0)

static char *child_msgs[3] = {
    "Hi from child apth 1\n",
    "Hi from child apth 2\n",
    "Hi from child apth 3\n",
};

static void *
thread_func(void *arg)
{
    int id = (int)arg;
    write(2, child_msgs[id], strlen(child_msgs[id]));
    return (void *)id;
}

APTH_CONFIG(cfg,
            cfg->workers = 4;)

APTH_MAIN_BEGIN(argc, argv)
{
    apth_t tids[3];
    void *cdatas[3];

    static char main_hi[] = "Hi from main apth\n";
    write(2, main_hi, sizeof(main_hi));

    for (int i = 0; i < 3; i++)
    {
        apth_attr_t child_attr;
        apth_attr_init(&child_attr);
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(i + 1, &cpuset);
        apth_attr_setaffinity_np(&child_attr, sizeof(cpuset), &cpuset);
        static char *child_names[3] = {
            "CHILD APTH 1",
            "CHILD APTH 2",
            "CHILD APTH 3",
        };
        apth_attr_setname_np(&child_attr, child_names[i]);

        apth_create(&tids[i], &child_attr, thread_func, (void *)i);
        apth_attr_destroy(&child_attr);
    }
    // sleep(2);
    for (int i = 0; i < 3; i++)
    {
        apth_join(tids[i], &cdatas[i]);
    }
    for (int i = 0; i < 3; i++)
    {
        if ((int)cdatas[i] != i)
        {

            static char err_other_msg[] = "child apth should yield identity, but not\n";
            write(2, err_other_msg, sizeof(err_other_msg));
        }
    }
}
APTH_MAIN_END
