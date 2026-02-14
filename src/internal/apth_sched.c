#include "internal_types.h"
#include "internal_funcs.h"
#include "utils/debug.h"
#include "utils/atomic_wrapper.h"
#include "utils/apth_errno.h"

// Total APTH threads we have. Note this counter is shared across the process,
// So it should be _Atomic.
_Atomic unsigned int apth_nthreads = 0;

// Initialize a scheduler onto `worker` and put the new scheduler in `sched`.
bool apth_scheduler_init(apth_sched_t sched, apth_worker_t worker)
{
    sched->id = worker->worker_id;

    if (pipe(sched->apth_sigpipe) == -1)
        return apth_error(false, errno);
    if (apth_fdmode(sched->apth_sigpipe[0], APTH_FDMODE_NONBLOCK) == APTH_FDMODE_ERROR)
        return apth_error(false, errno);
    if (apth_fdmode(sched->apth_sigpipe[1], APTH_FDMODE_NONBLOCK) == APTH_FDMODE_ERROR)
        return apth_error(false, errno);

    list_init(&sched->new_list);
    list_init(&sched->ready_list);
    list_init(&sched->waiting_list);
    list_init(&sched->suspended_list);
    list_init(&sched->terminated_list);
    sched->worker = worker;
    sched->switches = 0;
    sched->thrcnt = 0;
    apth_time_set(&sched->running, APTH_TIME_ZERO);
    sched->cur = APTH_NULL;

    // Initialize load support
    sched->loadval = 1.0;
    apth_time_set(&sched->apth_loadticknext, APTH_TIME_NOW);

    // Mark the scheduler as opening
    atomic_store_release(&sched->opening, true);
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
#define head_apth_of(name) head_apth_of_##name
#define new_apth_to(name) new_apth_to_##name
#define list_of(name) name##_list
// TODO: acquire list lock, since the caller pthread may not be ourself
// TODO: release list lock
#define DEFINE_SCHED_LIST_OP(name)                            \
    void push_apth_to(name)(apth_t th, apth_sched_t sched)    \
    {                                                         \
        assert(th->belongs_to_list == NULL);                  \
        list_push_back(&sched->list_of(name), &th->elem);     \
        th->belongs_to_list = &sched->list_of(name);          \
    }                                                         \
    apth_t pop_apth_from(name)(apth_sched_t sched)            \
    {                                                         \
        apth_t th = NULL;                                     \
        struct list_elem *e;                                  \
        assert(th->belongs_to_list == &sched->list_of(name)); \
        if (!list_empty(&sched->list_of(name)))               \
        {                                                     \
            e = list_pop_front(&sched->list_of(name));        \
            th = apth_t_list_entry(e);                        \
        }                                                     \
        th->belongs_to_list = NULL;                           \
        return th;                                            \
    }                                                         \
    apth_t head_apth_of(name)(apth_sched_t sched)             \
    {                                                         \
        apth_t th = NULL;                                     \
        struct list_elem *e;                                  \
        if (!list_empty(&sched->list_of(name)))               \
        {                                                     \
            e = list_front(&sched->list_of(name));            \
            th = apth_t_list_entry(e);                        \
        }                                                     \
        return th;                                            \
    }

DEFINE_SCHED_LIST_OP(new)
DEFINE_SCHED_LIST_OP(ready)
DEFINE_SCHED_LIST_OP(waiting)
DEFINE_SCHED_LIST_OP(suspended)
DEFINE_SCHED_LIST_OP(terminated)

#undef DEFINE_SCHED_LIST_OP
#undef list_of
#undef new_apth_to
#undef head_apth_of
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

// Drop all threads (except for the currently active one)
void apth_scheduler_drop(apth_sched_t sched)
{
#define CLEAR_T_LIST(name)                     \
    FOR_ELEMENT_IN_LIST(sched->name##_list, e) \
    {                                          \
        apth_t t = apth_t_list_entry(e);       \
        apth_tch_free(t);                      \
    }                                          \
    list_init(&sched->name##_list);

    // Clear the apth queues
    CLEAR_T_LIST(new);
    CLEAR_T_LIST(ready);
    CLEAR_T_LIST(waiting);
    CLEAR_T_LIST(suspended);
    CLEAR_T_LIST(terminated);
#undef CLEAR_T_LIST
    return;
}

// Kill the schduler ingredients
void apth_scheduler_kill(apth_sched_t sched)
{
    // Drop all apths
    apth_scheduler_drop(sched);

    // Remove the internal signal pipe
    close(sched->apth_sigpipe[0]);
    close(sched->apth_sigpipe[1]);

    // Mark the scheduler as closed
    atomic_store_release(&sched->opening, false);
    return;
}

static bool apth_is_not_null_and_valid(apth_t th)
{
    // Assert sanity
    apth_sched_t sched = cur_sched();
    struct list *sl;
    switch (th->state)
    {
    case APTH_STATE_NEW:
        sl = &sched->new_list;
        break;
    case APTH_STATE_READY:
        sl = &sched->ready_list;
        break;
    case APTH_STATE_WAITING:
        sl = &sched->waiting_list;
        break;
    case APTH_STATE_TERMINATED:
        sl = &sched->terminated_list;
        break;
    default:
        PANIC("should not reach here");
        break;
    }
    struct list *l = th->belongs_to_list;

    assert(l == sl);

    // List contains
    bool found = false;
    FOR_ELEMENT_IN_LIST_REF(l, e)
    {
        apth_t t = apth_t_list_entry(e);
        if (t == th)
            found = true;
    }
    return found;
}