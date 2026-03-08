#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif // _GNU_SOURCE
#include <sched.h>

#include "apth.h"
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define handle_error_en(en, msg) \
    do                           \
    {                            \
        errno = en;              \
        perror(msg);             \
        exit(EXIT_FAILURE);      \
    } while (0)

static char *child_msgs[1] = {
    "Hi from child apth 1\n",
};

static void *
thread_func(void *arg)
{
    int id = (int)(intptr_t)arg;
    write(2, child_msgs[id], strlen(child_msgs[id]));
    return (void *)(intptr_t)id;
}

APTH_CONFIG(cfg,
            cfg->workers = 4;)

APTH_MAIN_BEGIN(argc, argv)
{
    apth_t tids[1];
    void *cdatas[1];

    static char main_hi[] = "Hi from main apth\n";
    write(2, main_hi, sizeof(main_hi));

    for (int i = 0; i < 1; i++)
    {
        apth_attr_t child_attr;
        apth_attr_init(&child_attr);
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(i + 1, &cpuset);
        apth_attr_setaffinity_np(&child_attr, sizeof(cpuset), &cpuset);
        static char *child_names[1] = {
            "CHILD APTH 1",
        };
        apth_attr_setname_np(&child_attr, child_names[i]);

        apth_create(&tids[i], &child_attr, thread_func, (void *)(intptr_t)i);
        apth_attr_destroy(&child_attr);
    }
    // sleep(2);
    for (int i = 0; i < 1; i++)
    {
        apth_join(tids[i], &cdatas[i]);
    }
    for (int i = 0; i < 1; i++)
    {
        if ((int)(intptr_t)cdatas[i] != i)
        {

            static char err_other_msg[] = "child apth should yield identity, but not\n";
            if (write(2, err_other_msg, sizeof(err_other_msg)) < 0) {
                fprintf(stderr, "[FAIL] write() failed\n");
                return NULL;
            }
        }
    }
}
APTH_MAIN_END
