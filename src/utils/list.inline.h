#ifndef _LIBAPTH_UTILS_LIST_INLINE_H
#define _LIBAPTH_UTILS_LIST_INLINE_H

#include "list.h"

#include "debug.h"
#include "archplattoold.h"

/** Our doubly linked lists have two header elements: the "head"
   just before the first element and the "tail" just after the
   last element.  The `prev' link of the front header is null, as
   is the `next' link of the back header.  Their other two links
   point toward each other via the interior elements of the list.

   An empty list looks like this:

                      +------+     +------+
                  <---| head |<--->| tail |--->
                      +------+     +------+

   A list with two elements in it looks like this:

        +------+     +-------+     +-------+     +------+
    <---| head |<--->|   1   |<--->|   2   |<--->| tail |<--->
        +------+     +-------+     +-------+     +------+

   The symmetry of this arrangement eliminates lots of special
   cases in list processing.  For example, take a look at
   list_remove(): it takes only two pointer assignments and no
   conditionals.  That's a lot simpler than the code would be
   without header elements.

   (Because only one of the pointers in each header element is used,
   we could in fact combine them into a single header element
   without sacrificing this simplicity.  But using two separate
   elements allows us to do a little bit of checking on some
   operations, which can be valuable.) */


// ==================== Forward Declaration ====================

INLINE void list_init(struct list *);

/** List traversal. */
INLINE struct list_elem *list_begin(struct list *);
INLINE struct list_elem *list_next(struct list_elem *);
INLINE struct list_elem *list_end(struct list *);

INLINE struct list_elem *list_rbegin(struct list *);
INLINE struct list_elem *list_prev(struct list_elem *);
INLINE struct list_elem *list_rend(struct list *);

INLINE struct list_elem *list_head(struct list *);
INLINE struct list_elem *list_tail(struct list *);

/** List insertion. */
INLINE void list_insert(struct list_elem *, struct list_elem *);
INLINE void list_splice(
    struct list_elem *before,
    struct list_elem *first,
    struct list_elem *last);
INLINE void list_push_front(struct list *, struct list_elem *);
INLINE void list_push_back(struct list *, struct list_elem *);
INLINE struct list_elem *list_append(struct list *, struct list *);

/** List removal. */
INLINE struct list_elem *list_remove(struct list_elem *);
INLINE struct list_elem *list_pop_front(struct list *);
INLINE struct list_elem *list_pop_back(struct list *);

/** List elements. */
INLINE struct list_elem *list_front(struct list *);
INLINE struct list_elem *list_back(struct list *);

/** List properties. */
INLINE size_t list_size(struct list *);
INLINE bool list_empty(struct list *);

/** Miscellaneous. */
INLINE void list_reverse(struct list *);

/** Operations on lists with ordered elements. */
INLINE void list_sort(struct list *, list_less_func *, void *aux);
INLINE void list_insert_ordered(
    struct list *,
    struct list_elem *,
    list_less_func *,
    void *aux);
INLINE void list_unique(
    struct list *,
    struct list *duplicates,
    list_less_func *,
    void *aux);

/** Max and min. */
INLINE struct list_elem *list_max(struct list *, list_less_func *, void *aux);
INLINE struct list_elem *list_min(struct list *, list_less_func *, void *aux);

// ==================== Inline functions definition ====================

/** Swaps the `struct list_elem *'s that A and B point to. */
INLINE void __list_swap(struct list_elem **a, struct list_elem **b)
{
    struct list_elem *t = *a;
    *a = *b;
    *b = t;
}

/** Returns true if ELEM is a head, false otherwise. */
INLINE bool is_head(struct list_elem *elem)
{
    return elem != NULL && elem->prev == NULL && elem->next != NULL;
}

/** Returns true if ELEM is an interior element,
   false otherwise. */
INLINE bool is_interior(struct list_elem *elem)
{
    return elem != NULL && elem->prev != NULL && elem->next != NULL;
}

/** Returns true if ELEM is a tail, false otherwise. */
INLINE bool is_tail(struct list_elem *elem)
{
    return elem != NULL && elem->prev != NULL && elem->next == NULL;
}

/** Initializes LIST as an empty list. */
INLINE void list_init(struct list *list)
{
    assert(list != NULL);
    list->head.prev = NULL;
    list->head.next = &list->tail;
    list->tail.prev = &list->head;
    list->tail.next = NULL;
}

/** Returns the beginning of LIST.  */
INLINE struct list_elem *list_begin(struct list *list)
{
    assert(list != NULL);
    return list->head.next;
}

/** Returns the element after ELEM in its list.  If ELEM is the
   last element in its list, returns the list tail.  Results are
   undefined if ELEM is itself a list tail. */
INLINE struct list_elem *list_next(struct list_elem *elem)
{
    assert(is_head(elem) || is_interior(elem));
    return elem->next;
}

/** Returns LIST's tail.

   list_end() is often used in iterating through a list from
   front to back.  See the big comment at the top of list.h for
   an example. */
INLINE struct list_elem *list_end(struct list *list)
{
    assert(list != NULL);
    return &list->tail;
}

/** Returns the LIST's reverse beginning, for iterating through
   LIST in reverse order, from back to front. */
INLINE struct list_elem *list_rbegin(struct list *list)
{
    assert(list != NULL);
    return list->tail.prev;
}

/** Returns the element before ELEM in its list.  If ELEM is the
   first element in its list, returns the list head.  Results are
   undefined if ELEM is itself a list head. */
INLINE struct list_elem *list_prev(struct list_elem *elem)
{
    assert(is_interior(elem) || is_tail(elem));
    return elem->prev;
}

/** Returns LIST's head.

   list_rend() is often used in iterating through a list in
   reverse order, from back to front.  Here's typical usage,
   following the example from the top of list.h:

      for (e = list_rbegin (&foo_list); e != list_rend (&foo_list);
           e = list_prev (e))
        {
          struct foo *f = list_entry (e, struct foo, elem);
          ...do something with f...
        }
*/
INLINE struct list_elem *list_rend(struct list *list)
{
    assert(list != NULL);
    return &list->head;
}

/** Return's LIST's head.

   list_head() can be used for an alternate style of iterating
   through a list, e.g.:

      e = list_head (&list);
      while ((e = list_next (e)) != list_end (&list))
        {
          ...
        }
*/
INLINE struct list_elem *list_head(struct list *list)
{
    assert(list != NULL);
    return &list->head;
}

/** Return's LIST's tail. */
INLINE struct list_elem *list_tail(struct list *list)
{
    assert(list != NULL);
    return &list->tail;
}

/** Inserts ELEM just before BEFORE, which may be either an
   interior element or a tail.  The latter case is equivalent to
   list_push_back(). */
INLINE void list_insert(struct list_elem *before, struct list_elem *elem)
{
    assert(is_interior(before) || is_tail(before));
    assert(elem != NULL);

    elem->prev = before->prev;
    elem->next = before;
    before->prev->next = elem;
    before->prev = elem;
}

/** Removes elements FIRST though LAST (exclusive) from their
   current list, then inserts them just before BEFORE, which may
   be either an interior element or a tail. */
INLINE void list_splice(
    struct list_elem *before,
    struct list_elem *first,
    struct list_elem *last)
{
    assert(is_interior(before) || is_tail(before));
    if (first == last)
        return;
    last = list_prev(last);

    assert(is_interior(first));
    assert(is_interior(last));

    /* Cleanly remove FIRST...LAST from its current list. */
    first->prev->next = last->next;
    last->next->prev = first->prev;

    /* Splice FIRST...LAST into new list. */
    first->prev = before->prev;
    last->next = before;
    before->prev->next = first;
    before->prev = last;
}

/** Inserts ELEM at the beginning of LIST, so that it becomes the
   front in LIST. */
INLINE void list_push_front(struct list *list, struct list_elem *elem)
{
    list_insert(list_begin(list), elem);
}

/** Inserts ELEM at the end of LIST, so that it becomes the
   back in LIST. */
INLINE void list_push_back(struct list *list, struct list_elem *elem)
{
    list_insert(list_end(list), elem);
}

/** Transfers all elements from SECOND to the tail of FIRST, leaving SECOND
   empty.  Returns the element that was the front of SECOND before the
   transfer, or NULL if SECOND was empty. */
INLINE struct list_elem *list_append(struct list *first, struct list *second)
{
    assert(first != NULL);
    assert(second != NULL);

    if (list_empty(second))
        return NULL;

    struct list_elem *first_end = list_end(first);
    struct list_elem *sec_front = list_front(second);
    struct list_elem *sec_back  = list_back(second);

    sec_front->prev       = first_end->prev;
    sec_back->next        = first_end;
    first_end->prev->next = sec_front;
    first_end->prev       = sec_back;

    list_init(second);
    return sec_front;
}

/** Removes ELEM from its list and returns the element that
   followed it.  Undefined behavior if ELEM is not in a list.

   A list element must be treated very carefully after removing
   it from its list.  Calling list_next() or list_prev() on ELEM
   will return the item that was previously before or after ELEM,
   but, e.g., list_prev(list_next(ELEM)) is no longer ELEM!

   The list_remove() return value provides a convenient way to
   iterate and remove elements from a list:

   for (e = list_begin (&list); e != list_end (&list); e = list_remove (e))
     {
       ...do something with e...
     }

   If you need to free() elements of the list then you need to be
   more conservative.  Here's an alternate strategy that works
   even in that case:

   while (!list_empty (&list))
     {
       struct list_elem *e = list_pop_front (&list);
       ...do something with e...
     }
*/
INLINE struct list_elem *list_remove(struct list_elem *elem)
{
    assert(is_interior(elem));
    elem->prev->next = elem->next;
    elem->next->prev = elem->prev;
    return elem->next;
}

/** Removes the front element from LIST and returns it.
   Undefined behavior if LIST is empty before removal. */
INLINE struct list_elem *list_pop_front(struct list *list)
{
    struct list_elem *front = list_front(list);
    list_remove(front);
    return front;
}

/** Removes the back element from LIST and returns it.
   Undefined behavior if LIST is empty before removal. */
INLINE struct list_elem *list_pop_back(struct list *list)
{
    struct list_elem *back = list_back(list);
    list_remove(back);
    return back;
}

/** Returns the front element in LIST.
   Undefined behavior if LIST is empty. */
INLINE struct list_elem *list_front(struct list *list)
{
    assert(!list_empty(list));
    return list->head.next;
}

/** Returns the back element in LIST.
   Undefined behavior if LIST is empty. */
INLINE struct list_elem *list_back(struct list *list)
{
    assert(!list_empty(list));
    return list->tail.prev;
}

/** Returns the number of elements in LIST.
   Runs in O(n) in the number of elements. */
INLINE size_t list_size(struct list *list)
{
    struct list_elem *e;
    size_t cnt = 0;

    for (e = list_begin(list); e != list_end(list); e = list_next(e))
        cnt++;
    return cnt;
}

/** Returns true if LIST is empty, false otherwise. */
INLINE bool list_empty(struct list *list)
{
    return list_begin(list) == list_end(list);
}

/** Reverses the order of LIST. */
INLINE void list_reverse(struct list *list)
{
    if (!list_empty(list))
    {
        struct list_elem *e;

        for (e = list_begin(list); e != list_end(list); e = e->prev)
            __list_swap(&e->prev, &e->next);
        __list_swap(&list->head.next, &list->tail.prev);
        __list_swap(&list->head.next->prev, &list->tail.prev->next);
    }
}

/** Returns true only if the list elements A through B (exclusive)
   are in order according to LESS given auxiliary data AUX. */
INLINE bool __list_is_sorted(
    struct list_elem *a,
    struct list_elem *b,
    list_less_func *less,
    void *aux)
{
    if (a != b)
        while ((a = list_next(a)) != b)
            if (less(a, list_prev(a), aux))
                return false;
    return true;
}

/** Finds a run, starting at A and ending not after B, of list
   elements that are in nondecreasing order according to LESS
   given auxiliary data AUX.  Returns the (exclusive) end of the
   run.
   A through B (exclusive) must form a non-empty range. */
INLINE struct list_elem *__list_find_end_of_run(
    struct list_elem *a,
    struct list_elem *b,
    list_less_func *less,
    void *aux)
{
    assert(a != NULL);
    assert(b != NULL);
    assert(less != NULL);
    assert(a != b);

    do
    {
        a = list_next(a);
    } while (a != b && !less(a, list_prev(a), aux));
    return a;
}

/** Merges A0 through A1B0 (exclusive) with A1B0 through B1
   (exclusive) to form a combined range also ending at B1
   (exclusive).  Both input ranges must be nonempty and sorted in
   nondecreasing order according to LESS given auxiliary data
   AUX.  The output range will be sorted the same way. */
INLINE void __list_inplace_merge(
    struct list_elem *a0,
    struct list_elem *a1b0,
    struct list_elem *b1,
    list_less_func *less,
    void *aux)
{
    assert(a0 != NULL);
    assert(a1b0 != NULL);
    assert(b1 != NULL);
    assert(less != NULL);
    assert(__list_is_sorted(a0, a1b0, less, aux));
    assert(__list_is_sorted(a1b0, b1, less, aux));

    while (a0 != a1b0 && a1b0 != b1)
        if (!less(a1b0, a0, aux))
            a0 = list_next(a0);
        else
        {
            a1b0 = list_next(a1b0);
            list_splice(a0, list_prev(a1b0), a1b0);
        }
}

/** Sorts LIST according to LESS given auxiliary data AUX, using a
   natural iterative merge sort that runs in O(n lg n) time and
   O(1) space in the number of elements in LIST. */
INLINE void list_sort(struct list *list, list_less_func *less, void *aux)
{
    size_t output_run_cnt; /**< Number of runs output in current pass. */

    assert(list != NULL);
    assert(less != NULL);

    /* Pass over the list repeatedly, merging adjacent runs of
       nondecreasing elements, until only one run is left. */
    do
    {
        struct list_elem *a0;   /**< Start of first run. */
        struct list_elem *a1b0; /**< End of first run, start of second. */
        struct list_elem *b1;   /**< End of second run. */

        output_run_cnt = 0;
        for (a0 = list_begin(list); a0 != list_end(list); a0 = b1)
        {
            /* Each iteration produces one output run. */
            output_run_cnt++;

            /* Locate two adjacent runs of nondecreasing elements
               A0...A1B0 and A1B0...B1. */
            a1b0 = __list_find_end_of_run(a0, list_end(list), less, aux);
            if (a1b0 == list_end(list))
                break;
            b1 = __list_find_end_of_run(a1b0, list_end(list), less, aux);

            /* Merge the runs. */
            __list_inplace_merge(a0, a1b0, b1, less, aux);
        }
    } while (output_run_cnt > 1);

    assert(__list_is_sorted(list_begin(list), list_end(list), less, aux));
}

/** Inserts ELEM in the proper position in LIST, which must be
   sorted according to LESS given auxiliary data AUX.
   Runs in O(n) average case in the number of elements in LIST. */
INLINE void list_insert_ordered(
    struct list *list,
    struct list_elem *elem,
    list_less_func *less,
    void *aux)
{
    struct list_elem *e;

    assert(list != NULL);
    assert(elem != NULL);
    assert(less != NULL);

    for (e = list_begin(list); e != list_end(list); e = list_next(e))
        if (less(elem, e, aux))
            break;
    return list_insert(e, elem);
}

/** Iterates through LIST and removes all but the first in each
   set of adjacent elements that are equal according to LESS
   given auxiliary data AUX.  If DUPLICATES is non-null, then the
   elements from LIST are appended to DUPLICATES. */
INLINE void list_unique(
    struct list *list,
    struct list *duplicates,
    list_less_func *less,
    void *aux)
{
    struct list_elem *elem, *next;

    assert(list != NULL);
    assert(less != NULL);
    if (list_empty(list))
        return;

    elem = list_begin(list);
    while ((next = list_next(elem)) != list_end(list))
        if (!less(elem, next, aux) && !less(next, elem, aux))
        {
            list_remove(next);
            if (duplicates != NULL)
                list_push_back(duplicates, next);
        }
        else
            elem = next;
}

/** Returns the element in LIST with the largest value according
   to LESS given auxiliary data AUX.  If there is more than one
   maximum, returns the one that appears earlier in the list.  If
   the list is empty, returns its tail. */
INLINE struct list_elem *list_max(struct list *list, list_less_func *less, void *aux)
{
    struct list_elem *max = list_begin(list);
    if (max != list_end(list))
    {
        struct list_elem *e;

        for (e = list_next(max); e != list_end(list); e = list_next(e))
            if (less(max, e, aux))
                max = e;
    }
    return max;
}

/** Returns the element in LIST with the smallest value according
   to LESS given auxiliary data AUX.  If there is more than one
   minimum, returns the one that appears earlier in the list.  If
   the list is empty, returns its tail. */
INLINE struct list_elem *list_min(struct list *list, list_less_func *less, void *aux)
{
    struct list_elem *min = list_begin(list);
    if (min != list_end(list))
    {
        struct list_elem *e;

        for (e = list_next(min); e != list_end(list); e = list_next(e))
            if (less(e, min, aux))
                min = e;
    }
    return min;
}

#endif // _LIBAPTH_UTILS_LIST_INLINE_H
