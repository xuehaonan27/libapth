#include "internal_types.h"
#include "internal_funcs.h"
#include "utils/list.h"
#include "utils/debug.h"
#include "utils/atomic_wrapper.h"

APTH_INTERNAL apth_thqueue_t belonging_queue_of(apth_t th, const char *dbg_msg)
{
    apth_thqueue_t q = atomic_load_acquire(&th->belongs_to_queue);
    assert_msg(q != NULL, "calling belonging_queue_of from: %s", dbg_msg);
    return q;
}

APTH_INTERNAL void set_belonging_queue_of(apth_t th, apth_thqueue_t q)
{
    atomic_store_release(&th->belongs_to_queue, q);
    // atomic_store_seqcst(&th->belongs_to_queue, q);
}

APTH_INTERNAL void thqueue_init(apth_thqueue_t *queue_ptr, apth_sched_t sched, apth_state_t state)
{
    apth_thqueue_t queue;
    if ((queue = malloc(sizeof(struct apth_thqueue_st))) == NULL)
    {
        PANIC("nomem");
    }

    list_init(&queue->th_list);
    lll_init(&queue->th_list_lock);
    queue->sched = sched;
    queue->th_state = state;
    queue->size = 0;

    *queue_ptr = queue;
}

APTH_INTERNAL size_t thqueue_size(apth_thqueue_t queue)
{
    assert(queue != NULL);
    size_t s;
    lll_lock(&queue->th_list_lock, "thqueue_size");
    s = queue->size;
    lll_unlock(&queue->th_list_lock, "thqueue_size");
    return s;
}

APTH_INTERNAL void push_apth_to(apth_thqueue_t queue, apth_t th)
{
    assert(queue != NULL);
    assert(APTH_IS_VALID(th));
    // assert(belonging_queue_of(th, "push_apth_to") == NULL);
    assert(atomic_load_acquire(&th->belongs_to_queue) == NULL);

    lll_lock(&queue->th_list_lock, "push_apth_to");
    list_push_back(&queue->th_list, &th->elem);
    queue->size += 1;
    set_belonging_queue_of(th, queue);
    commit_state_of(th, queue->th_state);
    lll_unlock(&queue->th_list_lock, "push_apth_to");
}

APTH_INTERNAL apth_t pop_apth_from(apth_thqueue_t queue)
{
    assert(queue != NULL);
    apth_t th = APTH_NULL;
    struct list_elem *e;

    lll_lock(&queue->th_list_lock, "pop_apth_from");
    if (!list_empty(&queue->th_list))
    {
        e = list_front(&queue->th_list);
        th = apth_t_list_entry(e);
        assert(belonging_queue_of(th, "pop_apth_from") == queue);
        struct list_elem *ee = list_pop_front(&queue->th_list);
        assert(e == ee);
        queue->size -= 1;
    }

    if (th != APTH_NULL)
        set_belonging_queue_of(th, NULL);

    lll_unlock(&queue->th_list_lock, "pop_apth_from");

    return th;
}

APTH_INTERNAL bool apth_is_in(apth_thqueue_t queue, apth_t th)
{
    assert(queue != NULL);
    return (belonging_queue_of(th, "apth_is_in") == queue);
}

APTH_INTERNAL void remove_apth_from(apth_thqueue_t queue, apth_t th)
{
    assert(queue != NULL);
    assert(APTH_IS_VALID(th));
    assert(belonging_queue_of(th, "remove_apth_from") != NULL);
    assert_msg(belonging_queue_of(th, "remove_apth_from") == queue,
               "belonging_queue_of %p (\"%s\") = %p (state=%d) but got %p(state=%d)",
               th, th->name,
               belonging_queue_of(th, "remove_apth_from"),
               belonging_queue_of(th, "remove_apth_from")->th_state,
               queue, queue->th_state);
    lll_lock(&queue->th_list_lock, "remove_apth_from");
    list_remove(&th->elem);
    queue->size -= 1;
    set_belonging_queue_of(th, NULL);
    lll_unlock(&queue->th_list_lock, "remove_apth_from");
}

// Get the ownership of the queue and drain the with `fn`
APTH_INTERNAL void drain_thqueue(apth_thqueue_t queue, drain_thqueue_th_func fn)
{
    lll_lock(&queue->th_list_lock, "drain_thqueue");

    // Note: the entire process should be protected by lock
    struct list_elem *e = NULL;
    while (!list_empty(&queue->th_list))
    {
        // e = list_pop_front(&unlinked_list);
        e = list_front(&queue->th_list);
        apth_t th = apth_t_list_entry(e);
        assert(APTH_IS_VALID(th));
        assert(belonging_queue_of(th, "drain_thqueue") != NULL);
        assert(belonging_queue_of(th, "drain_thqueue") == queue);
        struct list_elem *ee = list_pop_front(&queue->th_list);
        assert(e == ee);

        set_belonging_queue_of(th, NULL);
        fn(th);
    }

    queue->size = 0;

    lll_unlock(&queue->th_list_lock, "drain_thqueue");
}

APTH_INTERNAL apth_t transfer_one_th(apth_thqueue_t from, apth_thqueue_t to,
                                     bool insert_from_front, const char *dbg_msg)
{
    assert(from != NULL);
    assert(to != NULL);
    struct list_elem *e = NULL;
    apth_t th = APTH_NULL;

    // First we should remove `th` from `from`
    lll_lock(&from->th_list_lock, "transfer_th locking from");
    if (list_empty(&from->th_list))
    {
        assert(from->size == 0);
        lll_unlock(&from->th_list_lock, "transfer_th unlocking from");
        return th;
    }
    e = list_pop_front(&from->th_list);
    from->size -= 1;
    lll_unlock(&from->th_list_lock, "transfer_th unlocking from");

    th = apth_t_list_entry(e);
    assert(APTH_IS_VALID(th));
    assert(belonging_queue_of(th, dbg_msg) != NULL);
    assert(belonging_queue_of(th, dbg_msg) == from);

    submit_desired_state_to(th, to->th_state, dbg_msg);

    // Then we should push `th` to `to`
    lll_lock(&to->th_list_lock, "transfer_th locking to");
    if (insert_from_front)
        list_push_front(&to->th_list, &th->elem);
    else
        list_push_back(&to->th_list, &th->elem);
    to->size += 1;
    set_belonging_queue_of(th, to);
    // Till now, state of `th` should still be former value
    commit_state_of(th, to->th_state);
    lll_unlock(&to->th_list_lock, "transfer_th unlocking to");

    return th;
}

APTH_INTERNAL void transfer_th(apth_t th, apth_thqueue_t from, apth_thqueue_t to)
{
    assert(APTH_IS_VALID(th));
    assert(from != NULL);
    assert(to != NULL);
    assert(belonging_queue_of(th, "transfer_th") != NULL);
    assert(belonging_queue_of(th, "transfer_th") == from);

    // First we should remove `th` from `from`
    lll_lock(&from->th_list_lock, "transfer_th locking from");
    list_remove(&th->elem);
    from->size -= 1;
    lll_unlock(&from->th_list_lock, "transfer_th unlocking from");

    // submit_desired_state_to(th, to->th_state);

    // Then we should push `th` to `to`
    lll_lock(&to->th_list_lock, "transfer_th locking to");
    list_push_back(&to->th_list, &th->elem);
    to->size += 1;
    set_belonging_queue_of(th, to);
    commit_state_of(th, to->th_state);
    lll_unlock(&to->th_list_lock, "transfer_th unlocking to");
}

// TODO: remove this
APTH_INTERNAL void transfer_thqueue(apth_thqueue_t queue_1, apth_thqueue_t queue_2)
{
    assert(queue_1 != NULL);
    assert(queue_2 != NULL);

    // // First get all elements from queue 2
    // lll_lock(&queue_2->th_list_lock, "transfer_thqueue");
    // struct list tmp_q2 = list_unlink(&queue_2->th_list);
    // size_t tmp_q2_size = queue_2->size;
    // queue_2->size = 0;
    // lll_unlock(&queue_2->th_list_lock, "transfer_thqueue");

    // // Transfer queue
    // lll_lock(&queue_1->th_list_lock, "transfer_thqueue");

    // struct list_elem *e = list_append(&queue_1->th_list, &tmp_q2);
    // queue_1->size += tmp_q2_size;

    // // After transferred, then we should modify belonging states
    // if (e != NULL)
    // {
    //     for (; e != list_end(&queue_1->th_list); e = list_next(e))
    //     {
    //         apth_t th = apth_t_list_entry(e);
    //         submit_desired_state_to(th, queue_1->th_state);
    //         set_belonging_queue_of(th, queue_1);
    //         commit_state_of(th, queue_1->th_state);
    //     }
    // }

    // lll_unlock(&queue_1->th_list_lock, "transfer_thqueue");
}

APTH_INTERNAL apth_t find_first_in_thqueue(apth_thqueue_t queue, find_first_in_thqueue_th_func fn, void *aux)
{
    assert(queue != NULL);

    apth_t ret_th = APTH_NULL;

    lll_lock(&queue->th_list_lock, "find_first_in_thqueue");
    FOR_ELEMENT_IN_LIST(queue->th_list, e)
    {
        apth_t th = apth_t_list_entry(e);

        assert(APTH_IS_VALID(th));
        assert(belonging_queue_of(th, "find_first_in_thqueue") != NULL);
        assert(belonging_queue_of(th, "find_first_in_thqueue") == queue);

        if (fn(th, aux))
        {
            ret_th = th;
            break;
        }
    }
    lll_unlock(&queue->th_list_lock, "find_first_in_thqueue");

    return ret_th;
}

// // Visit the queue and remove some of it according to return value of `fn`
APTH_INTERNAL size_t visit_thqueue(apth_thqueue_t queue, visit_thqueue_th_func fn, void *aux)
{
    assert(queue != NULL);

    struct list ret_list;
    list_init(&ret_list);

    lll_lock(&queue->th_list_lock, "visit_thqueue");

    apth_thqueue_t last_to_queue = NULL;
    apth_t th_last = APTH_NULL;

    size_t ret = 0;

    FOR_ELEMENT_IN_LIST(queue->th_list, e)
    {

        // NOTE: deadlock threat. But since we are currently only using
        // this `visit_thqueue` function when going over waiting queue,
        // which is private to scheduler, no other else is going to acquire
        // waiting queue lock, so it's okay.
#define HANDLE_MOVE_TH                                                              \
    if (th_last != APTH_NULL)                                                       \
    {                                                                               \
        assert_msg(belonging_queue_of(th_last, "visit_thqueue") == queue,           \
                   "belonging_queue_of %p (\"%s\") = %p"                            \
                   "(state=%d) but got %p(state=%d)",                               \
                   th_last, th_last->name,                                          \
                   belonging_queue_of(th_last, "visit_thqueue"),                    \
                   belonging_queue_of(th_last, "visit_thqueue")->th_state,          \
                   queue, queue->th_state);                                         \
        list_remove(&th_last->elem);                                                \
        queue->size -= 1;                                                           \
        submit_desired_state_to(th_last, last_to_queue->th_state, "visit_thqueue"); \
        lll_lock(&last_to_queue->th_list_lock, "visit_thqueue");                    \
        list_push_back(&last_to_queue->th_list, &th_last->elem);                    \
        last_to_queue->size += 1;                                                   \
        set_belonging_queue_of(th_last, last_to_queue);                             \
        commit_state_of(th_last, last_to_queue->th_state);                          \
        lll_unlock(&last_to_queue->th_list_lock, "visit_thqueue");                  \
        th_last = APTH_NULL;                                                        \
    }
        HANDLE_MOVE_TH
        apth_t th = apth_t_list_entry(e);

        assert(APTH_IS_VALID(th));
        assert(belonging_queue_of(th, "visit_thqueue") != NULL);
        assert(belonging_queue_of(th, "visit_thqueue") == queue);

        last_to_queue = fn(th, aux);
        if (last_to_queue == APTH_DONT_MOVE_BUT_COUNT)
            ret += 1;
        else if (last_to_queue != NULL)
        {
            ret += 1;
            th_last = th;
        }
    }

    HANDLE_MOVE_TH
#undef HANDLE_MOVE_TH
    lll_unlock(&queue->th_list_lock, "visit_thqueue");

    return ret;
}