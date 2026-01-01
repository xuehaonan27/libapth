#include "internal_types.h"
#include "debug.h"

// Total APTH threads we have. Note this counter is shared across the process,
// So it should be _Atomic.
_Atomic unsigned int apth_nthreads = 0;

// Initialize a scheduler onto `worker` and put the new scheduler in `sched`.
void apth_scheduler_init(apth_sched_t sched, apth_worker_t worker)
{
    sched->id = worker->worker_id;
    // TODO: initialize sched_ctx
    list_init(&sched->new_list);
    list_init(&sched->ready_list);
    list_init(&sched->waiting_list);
    list_init(&sched->suspended_list);
    list_init(&sched->terminated_list);
    sched->worker = worker;
    sched->switches = 0;
    sched->thrcnt = 0;
    sched->running = NULL;
}

void inc_thrcnt(apth_sched_t sched)
{
    sched->thrcnt += 1;
}

void dec_thrcnt(apth_sched_t sched)
{
    unsigned int c = sched->thrcnt;
    sched->thrcnt = c == 0 ? c : c - 1;
}

// void push_apth_to_ready(struct apth_t_list_elem *telem, apth_sched_t sched)
// {
//     // TODO: acquire list lock, since the caller pthread may not be ourself
//     list_push_back(&sched->ready_list, &telem->elem);
//     // TODO: release list lock
// }

// int new_apth_to_ready(apth_t t, apth_sched_t sched)
// {
//     struct apth_t_list_elem *telem = (struct apth_t_list_elem *)malloc(sizeof(struct apth_t_list_elem));
//     // TODO: handle OOM with apth_error
//     telem->ptcb = t;
//     push_apth_to_ready(telem, sched);
//     return 0;
// }

#define push_apth_to(name) push_apth_to_##name
#define new_apth_to(name) new_apth_to_##name
#define list_of(name) name##_list
// TODO: acquire list lock, since the caller pthread may not be ourself
// TODO: release list lock
#define DEFINE_SCHED_LIST_OP(name)                                                                \
    void push_apth_to(name)(struct apth_t_list_elem * telem, apth_sched_t sched)                  \
    {                                                                                             \
        list_push_back(&sched->list_of(name), &telem->elem);                                      \
    }                                                                                             \
    int new_apth_to(name)(apth_t t, apth_sched_t sched)                                           \
    {                                                                                             \
        struct apth_t_list_elem *telem;                                                           \
        if ((telem = (struct apth_t_list_elem *)malloc(sizeof(struct apth_t_list_elem))) == NULL) \
            return apth_error(-1, ENOMEM);                                                        \
        telem->ptcb = t;                                                                          \
        push_apth_to(name)(telem, sched);                                                         \
        return 0;                                                                                 \
    }

DEFINE_SCHED_LIST_OP(new)
DEFINE_SCHED_LIST_OP(ready)
DEFINE_SCHED_LIST_OP(waiting)
DEFINE_SCHED_LIST_OP(suspended)
DEFINE_SCHED_LIST_OP(terminated)

#undef DEFINE_SCHED_LIST_OP
#undef push_apth_to
#undef new_apth_to
#undef list_of
