#include "ring.h"

#include "debug.h"

void ring_init(struct ring *r)
{
    if (r == NULL)
        return;
    r->hook = NULL;
    r->elemcnt = 0;
    return;
}

// Return the first node in ring
#define ring_first(r) \
    ((r) == NULL ? NULL : (r)->hook)

// Return the last node in ring
#define ring_last(r) \
    ((r) == NULL ? NULL : ((r)->hook == NULL ? NULL : (r)->hook->prev))

// Walk to next node in ring
#define ring_next(r, rn) \
    (((r) == NULL || (rn) == NULL) ? NULL : ((rn)->next == (r)->hook ? NULL : (rn)->next))

// Walk to previous node in ring
#define ring_prev(r, rn) \
    (((r) == NULL || (rn) == NULL) ? NULL : ((rn)->prev == (r)->hook->prev ? NULL : (rn)->prev))

void ring_remove(struct ring *r, struct ring_elem *elem)
{
    if (r == NULL || elem == NULL)
        return;

    if (r->hook == elem && elem->prev == elem && elem->next == elem)
    {
        // The last element in the ring
        r->hook = NULL;
    }
    else
    {
        // Normal situation
        if (r->hook == elem)
            r->hook = elem->next;
        elem->prev->next = elem->next;
        elem->next->prev = elem->prev;
    }
    r->elemcnt -= 1;
    return;
}

void ring_push_back(struct ring *r, struct ring_elem *elem)
{
    if (r == NULL || elem == NULL)
        return;
    if (r->hook == NULL)
    {
        r->hook = elem;
        elem->next = elem;
        elem->prev = elem;
    }
    else
    {
        elem->next = r->hook;
        elem->prev = r->hook->prev;
        elem->next->prev = elem;
        elem->prev->next = elem;
    }
    r->elemcnt += 1;
    return;
}

void ring_push_front(struct ring *r, struct ring_elem *elem)
{
    if (r == NULL || elem == NULL)
        return;
    if (r->hook == NULL)
    {
        r->hook = elem;
        elem->next = elem;
        elem->prev = elem;
    }
    else
    {
        elem->next = r->hook;
        elem->prev = r->hook->prev;
        elem->next->prev = elem;
        elem->prev->next = elem;
        r->hook = elem;
    }
    r->elemcnt += 1;
    return;
}

struct ring_elem *ring_pop_back(struct ring *r, struct ring_elem *elem)
{
    struct ring_elem *elem;
    elem = ring_last(r);
    if (elem != NULL)
        ring_remove(r, elem);
    return elem;
}

struct ring_elem *ring_pop_front(struct ring *r, struct ring_elem *elem)
{
    struct ring_elem *elem;
    elem = ring_first(r);
    if (elem != NULL)
        ring_remove(r, elem);
    return elem;
}

bool ring_favorite(struct ring *r, struct ring_elem *elem)
{
    if (r == NULL)
        return false;
    if (r->hook == NULL)
        return false;
    if (r->hook == elem)
        return true;
    // move to hook of ring
    ring_remove(r, elem);
    ring_push_front(r, elem);
    return true;
}

// Check whether an element is contained in ring
int ring_contains(struct ring *r, struct ring_elem *elem)
{
    struct ring_elem *iter;
    int ret_val;
    if (r == NULL || elem == NULL)
        return apth_error(false, EINVAL);

    ret_val = false;

    iter = r->hook;
    if (iter != NULL)
    {
        do
        {
            if (iter == elem)
            {
                ret_val = true;
                break;
            }
            iter = iter->next;
        } while (iter != r->hook);
    }
    return ret_val;
}

size_t ring_size(struct ring *r) { return r->elemcnt; }
bool ring_empty(struct ring *r) { return r->hook == NULL; }
