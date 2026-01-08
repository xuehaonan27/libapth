#include "internal_types.h"
#include "debug.h"
#include "atomic_wrapper.h"

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

struct apth_t_list_elem *pop_apth_from_new(apth_sched_t sched)
{
    struct apth_t_list_elem *telem = NULL;
    struct list_elem *elem;
    if (!list_empty(&sched->new_list))
    {
        elem = list_pop_front(&sched->new_list);
        telem = apth_t_list_entry(elem);
    }
    return telem;
}

#define push_apth_to(name) push_apth_to_##name
#define pop_apth_from(name) pop_apth_from_##name
#define new_apth_to(name) new_apth_to_##name
#define list_of(name) name##_list
// TODO: acquire list lock, since the caller pthread may not be ourself
// TODO: release list lock
#define DEFINE_SCHED_LIST_OP(name)                                                                \
    void push_apth_to(name)(struct apth_t_list_elem * telem, apth_sched_t sched)                  \
    {                                                                                             \
        list_push_back(&sched->list_of(name), &telem->elem);                                      \
    }                                                                                             \
    struct apth_t_list_elem *pop_apth_from(name)(apth_sched_t sched)                              \
    {                                                                                             \
        struct apth_t_list_elem *telem = NULL;                                                    \
        struct list_elem *elem;                                                                   \
        if (!list_empty(&sched->list_of(name)))                                                   \
        {                                                                                         \
            elem = list_pop_front(&sched->list_of(name));                                         \
            telem = apth_t_list_entry(elem);                                                      \
        }                                                                                         \
        return telem;                                                                             \
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
#undef list_of
#undef new_apth_to
#undef pop_apth_from
#undef push_apth_to

bool apth_sched_is_opening(apth_sched_t sched)
{
    return atomic_load_relaxed(&sched->opening);
}
