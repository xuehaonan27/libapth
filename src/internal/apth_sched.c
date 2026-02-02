#include "internal_types.h"
#include "internal_funcs.h"
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
    sched->cur = APTH_NULL;

    // initialize load support
    sched->loadval = 1.0;
    apth_time_set(&sched->apth_loadticknext, APTH_TIME_NOW);
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

#define push_apth_to(name) push_apth_to_##name
#define pop_apth_from(name) pop_apth_from_##name
#define new_apth_to(name) new_apth_to_##name
#define list_of(name) name##_list
// TODO: acquire list lock, since the caller pthread may not be ourself
// TODO: release list lock
#define DEFINE_SCHED_LIST_OP(name)                         \
    void push_apth_to(name)(apth_t th, apth_sched_t sched) \
    {                                                      \
        list_push_back(&sched->list_of(name), &th->elem);  \
    }                                                      \
    apth_t pop_apth_from(name)(apth_sched_t sched)         \
    {                                                      \
        apth_t th = NULL;                                  \
        struct list_elem *e;                               \
        if (!list_empty(&sched->list_of(name)))            \
        {                                                  \
            e = list_pop_front(&sched->list_of(name));     \
            th = apth_t_list_entry(e);                     \
        }                                                  \
        return th;                                         \
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

static apth_time_t apth_loadtickgap = APTH_TIME(1, 0);

void apth_sched_calc_load(apth_sched_t sched, apth_time_t *now)
{
    if (apth_time_cmp(now, &sched->apth_loadticknext) >= 0)
    {
        apth_time_t ttmp;
        int numready = list_size(&sched->ready_list);
        apth_time_set(&ttmp, now);
        do
        {
            sched->loadval = (numready * 0.25) + (sched->loadval * 0.75);
            apth_time_sub(&ttmp, &apth_loadtickgap);
        } while (apth_time_cmp(&ttmp, &sched->apth_loadticknext) >= 0);
        apth_time_set(&sched->apth_loadticknext, now);
        apth_time_add(&sched->apth_loadticknext, &apth_loadtickgap);
    }
}