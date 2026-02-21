#define _GNU_SOURCE
#include <sched.h>
#define _POSIX_C_SOURCE 200809L
#include <signal.h>

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

    sigset_t sigs;
    sigemptyset(&sigs);
    // For child 1, we do not set signal mask
    // For child 2, we block SIGUSR1
    // For child 3, we block SIGUSR1 and SIGUSR2
    // And MAIN should send SIGUSR1 and SIGUSR2 to 3 children
    switch (id)
    {
    case 0: // child 1
        break;
    case 1: // child 2
        sigaddset(&sigs, SIGUSR1);
        break;
    case 2: // child 2
        sigaddset(&sigs, SIGUSR1);
        sigaddset(&sigs, SIGUSR2);
        break;
    default:
        abort(); // should not reach here
        break;
    }
    apth_sigmask(SIG_BLOCK, &sigs, NULL);

    static char *received_SIGUSR1[3] = {
        "child apth 1 received SIGUSR1\n",
        "child apth 2 received SIGUSR1\n",
        "child apth 3 received SIGUSR1\n",
    };

    static char *received_SIGUSR2[3] = {
        "child apth 1 received SIGUSR2\n",
        "child apth 2 received SIGUSR2\n",
        "child apth 3 received SIGUSR2\n",
    };

    sigemptyset(&sigs);
    sigaddset(&sigs, SIGUSR1);
    sigaddset(&sigs, SIGUSR2);

    int waited_sig;
    sigwait(&sigs, &waited_sig);

    if (waited_sig == SIGUSR1)
        write(2, received_SIGUSR1[id], strlen(received_SIGUSR1[id]));
    else if (waited_sig == SIGUSR2)
        write(2, received_SIGUSR2[id], strlen(received_SIGUSR2[id]));

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

    // Kill SIGUSR1 and SIGUSR2 to all 3 children
    for (int i = 0; i < 3; i++)
    {
        apth_kill(tids[i], SIGUSR1);
        apth_kill(tids[i], SIGUSR2);
    }

    sleep(5); // For all children have enough time to receive signal

    for (int i = 0; i < 3; i++)
    {
        apth_cancel(tids[i]);
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
