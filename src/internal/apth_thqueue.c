#include "apth_thqueue.h"
#include "internal/apth_tcb.h"
#include "utils/list.h"
#include "utils/debug.h"
#include "utils/atomic_wrapper.h"
#include "utils/lll.inline.h"

APTH_INTERNAL void thqueue_init(apth_thqueue_t queue, apth_state_t state)
{
    // apth_thqueue_t queue;
    // if ((queue = malloc(sizeof(struct apth_thqueue_st))) == NULL)
    // {
    //     PANIC("nomem");
    // }

    list_init(&queue->th_list);
    lll_internal_init(&queue->th_list_lock);
    queue->th_state = state;
    queue->size = 0;
}

APTH_INTERNAL void push_apth_to(apth_thqueue_t queue, apth_t th)
{
    assert(queue != NULL);
    assert(APTH_IS_VALID(th));

    lll_internal_lock(&queue->th_list_lock);
    list_push_back(&queue->th_list, &th->elem);
    queue->size++;
    // set_belonging_queue_of(th, queue);
    th->current_queue = queue;
    atomic_store_release(&th->state, queue->th_state);
    lll_internal_unlock(&queue->th_list_lock);
}

APTH_INTERNAL apth_t pop_apth_from(apth_thqueue_t queue)
{
    assert(queue != NULL);
    apth_t th = APTH_NULL;
    struct list_elem *e;

    lll_internal_lock(&queue->th_list_lock);
    if (!list_empty(&queue->th_list))
    {
        e = list_front(&queue->th_list);
        th = apth_t_list_entry(e);
        assert(th->current_queue == queue);
        struct list_elem *ee = list_pop_front(&queue->th_list);
        assert(e == ee);
        queue->size--;
    }

    if (th != APTH_NULL)
    {
        // set_belonging_queue_of(th, NULL);
        th->current_queue = NULL;
    }

    lll_internal_unlock(&queue->th_list_lock);

    return th;
}

APTH_INTERNAL void remove_apth_from(apth_thqueue_t queue, apth_t th)
{
    assert(queue != NULL);
    assert(APTH_IS_VALID(th));
    assert(th->current_queue != NULL);
    assert_msg(th->current_queue == queue,
               "current queue of %p (\"%s\") = %p (state=%d) but got %p(state=%d)",
               th, th->name,
               th->current_queue,
               th->current_queue->th_state,
               queue, queue->th_state);
    lll_internal_lock(&queue->th_list_lock);
    list_remove(&th->elem);
    queue->size--;
    // set_belonging_queue_of(th, NULL);
    th->current_queue = NULL;
    lll_internal_unlock(&queue->th_list_lock);
}

// Get the ownership of the queue and drain the with `fn`
APTH_INTERNAL void drain_thqueue(apth_thqueue_t queue, drain_thqueue_th_func fn)
{
    lll_internal_lock(&queue->th_list_lock);

    // Note: the entire process should be protected by lock
    struct list_elem *e = NULL;
    while (!list_empty(&queue->th_list))
    {
        e = list_front(&queue->th_list);
        apth_t th = apth_t_list_entry(e);
        assert(APTH_IS_VALID(th));
        assert(th->current_queue != NULL);
        assert(th->current_queue == queue);
        struct list_elem *ee = list_pop_front(&queue->th_list);
        assert(e == ee);

        // set_belonging_queue_of(th, NULL);
        th->current_queue = NULL;
        fn(th);
    }

    queue->size = 0;

    lll_internal_unlock(&queue->th_list_lock);
}

APTH_INTERNAL apth_t transfer_one_th(apth_thqueue_t from, apth_thqueue_t to,
                                     bool insert_from_front, const char *dbg_msg)
{
    (void)dbg_msg;
    assert(from != NULL);
    assert(to != NULL);
    struct list_elem *e = NULL;
    apth_t th = APTH_NULL;

    // First we should remove `th` from `from`
    lll_internal_lock(&from->th_list_lock);
    lll_internal_lock(&to->th_list_lock);
    if (list_empty(&from->th_list))
    {
        assert_msg(from->size == 0, "from->size == %d", from->size);
        lll_internal_unlock(&to->th_list_lock);
        lll_internal_unlock(&from->th_list_lock);
        return th;
    }
    e = list_pop_front(&from->th_list);
    from->size--;

    th = apth_t_list_entry(e);
    assert(APTH_IS_VALID(th));
    assert(th->current_queue != NULL);
    assert(th->current_queue == from);

    // Then we should push `th` to `to`
    if (insert_from_front)
        list_push_front(&to->th_list, &th->elem);
    else
        list_push_back(&to->th_list, &th->elem);
    to->size++;
    // set_belonging_queue_of(th, to);
    th->current_queue = to;
    atomic_store_release(&th->state, to->th_state);
    lll_internal_unlock(&to->th_list_lock);
    lll_internal_unlock(&from->th_list_lock);

    return th;
}

APTH_INTERNAL void transfer_th(apth_t th, apth_thqueue_t from, apth_thqueue_t to)
{
    assert(APTH_IS_VALID(th));
    assert(from != NULL);
    assert(to != NULL);
    assert(th->current_queue != NULL);
    assert(th->current_queue == from);

    // First we should remove `th` from `from`
    lll_internal_lock(&from->th_list_lock);
    lll_internal_lock(&to->th_list_lock);
    list_remove(&th->elem);
    from->size--;

    // Then we should push `th` to `to`
    list_push_back(&to->th_list, &th->elem);
    to->size++;
    // set_belonging_queue_of(th, to);
    th->current_queue = to;
    atomic_store_release(&th->state, to->th_state);
    lll_internal_unlock(&from->th_list_lock);
    lll_internal_unlock(&to->th_list_lock);
}

APTH_INTERNAL apth_t find_first_in_thqueue(apth_thqueue_t queue, find_first_in_thqueue_th_func fn, void *aux)
{
    assert(queue != NULL);

    apth_t ret_th = APTH_NULL;

    lll_internal_lock(&queue->th_list_lock);
    FOR_ELEMENT_IN_LIST(queue->th_list, e)
    {
        apth_t th = apth_t_list_entry(e);

        assert(APTH_IS_VALID(th));
        assert(th->current_queue != NULL);
        assert(th->current_queue == queue);

        if (fn(th, aux))
        {
            ret_th = th;
            break;
        }
    }
    lll_internal_unlock(&queue->th_list_lock);

    return ret_th;
}

// // Visit the queue and remove some of it according to return value of `fn`
APTH_INTERNAL size_t visit_thqueue(apth_thqueue_t queue, visit_thqueue_th_func fn, void *aux)
{
    assert(queue != NULL);

    struct list ret_list;
    list_init(&ret_list);

    lll_internal_lock(&queue->th_list_lock);

    apth_thqueue_t last_to_queue = NULL;
    apth_t th_last = APTH_NULL;

    size_t ret = 0;

    FOR_ELEMENT_IN_LIST(queue->th_list, e)
    {

#define HANDLE_MOVE_TH                                                  \
    if (th_last != APTH_NULL)                                           \
    {                                                                   \
        assert_msg(th_last->current_queue == queue,                     \
                   "current queue of %p (\"%s\") = %p"                  \
                   "(state=%d) but got %p(state=%d)",                   \
                   th_last, th_last->name,                              \
                   th_last->current_queue,                              \
                   th_last->current_queue->th_state,                    \
                   queue, queue->th_state);                             \
        list_remove(&th_last->elem);                                    \
        queue->size--;                                                  \
        lll_internal_lock(&last_to_queue->th_list_lock);                \
        list_push_back(&last_to_queue->th_list, &th_last->elem);        \
        last_to_queue->size++;                                          \
        th_last->current_queue = last_to_queue;                         \
        atomic_store_release(&th_last->state, last_to_queue->th_state); \
        lll_internal_unlock(&last_to_queue->th_list_lock);              \
        th_last = APTH_NULL;                                            \
    }
        HANDLE_MOVE_TH
        apth_t th = apth_t_list_entry(e);

        assert(APTH_IS_VALID(th));
        assert(th->current_queue != NULL);
        assert(th->current_queue == queue);

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
    lll_internal_unlock(&queue->th_list_lock);

    return ret;
}