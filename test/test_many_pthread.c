#define _GNU_SOURCE
#include <sched.h>
#include <pthread.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>

#define N_CHILDREN 200000
#define N_WORKERS 4

#define handle_error_en(en, msg) \
    do                           \
    {                            \
        errno = en;              \
        perror(msg);             \
        exit(EXIT_FAILURE);      \
    } while (0)

static void *
thread_func(void *arg)
{
    int id = (int)arg;
    char child_msg_buffer[1024];

    snprintf(child_msg_buffer, sizeof(child_msg_buffer), "CHILD APTH %d", id);
    pthread_setname_np(pthread_self(), child_msg_buffer);

    int retlen = snprintf(
        child_msg_buffer, sizeof(child_msg_buffer),
        "Hi from child apth %d\n", id);
    write(2, child_msg_buffer, retlen);
    return (void *)id;
}

pthread_t tids[N_CHILDREN];
void *cdatas[N_CHILDREN];

int main(int argc, char **argv)
{
    static char main_hi[] = "Hi from main apth\n";

    // TODO: remove this
    fcntl(2, F_SETFL, O_NONBLOCK);

    write(2, main_hi, sizeof(main_hi));

    for (int i = 0; i < N_CHILDREN; i++)
    {
        pthread_attr_t child_attr;
        pthread_attr_init(&child_attr);
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        int affinity_id = i % N_WORKERS;
        CPU_SET(affinity_id, &cpuset);
        pthread_attr_setaffinity_np(&child_attr, sizeof(cpuset), &cpuset);

        pthread_attr_setstacksize(&child_attr, 2048);

        // static char child_name_buffer[1024];
        // snprintf(child_name_buffer, sizeof(child_name_buffer), "CHILD APTH %d", i + 1);
        // pthread_attr_setname_np(&child_attr, child_name_buffer);

        pthread_create(&tids[i], &child_attr, thread_func, (void *)i);
        pthread_attr_destroy(&child_attr);
    }
    // sleep(2);
    for (int i = 0; i < N_CHILDREN; i++)
    {
        pthread_join(tids[i], &cdatas[i]);
    }
    for (int i = 0; i < N_CHILDREN; i++)
    {
        if ((int)cdatas[i] != i)
        {

            static char err_other_msg[] = "child apth should yield identity, but not\n";
            write(2, err_other_msg, sizeof(err_other_msg));
        }
    }
}
