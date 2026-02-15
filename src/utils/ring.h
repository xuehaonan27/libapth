#ifndef __LIBAPTH_UTILS_RING_H
#define __LIBAPTH_UTILS_RING_H

#include "archplattoold.h"
#include <stdbool.h>
#include <stddef.h>

struct ring_elem
{
    struct ring_elem *prev;
    struct ring_elem *next;
};

struct ring
{
    struct ring_elem *hook; // Hook to one element in the ring
    size_t elemcnt;         // How many nodes are there in this ring
};

/** Converts pointer to list element RING_ELEM into a pointer to
   the structure that RING_ELEM is embedded inside.  Supply the
   name of the outer structure STRUCT and the member name MEMBER
   of the list element.*/
#define ring_entry(RING_ELEM, STRUCT, MEMBER) \
    ((STRUCT *)((uint8_t *)&(RING_ELEM)->next - offsetof(STRUCT, MEMBER.next)))

APTH_INTERNAL void ring_init(struct ring *);
APTH_INTERNAL void ring_remove(struct ring *, struct ring_elem *);
APTH_INTERNAL void ring_push_back(struct ring *, struct ring_elem *);
APTH_INTERNAL void ring_push_front(struct ring *, struct ring_elem *);
APTH_INTERNAL struct ring_elem *ring_pop_back(struct ring *);
APTH_INTERNAL struct ring_elem *ring_pop_front(struct ring *);
APTH_INTERNAL bool ring_favorite(struct ring *, struct ring_elem *);
APTH_INTERNAL bool ring_contains(struct ring *, struct ring_elem *);
APTH_INTERNAL size_t ring_size(struct ring *);
APTH_INTERNAL bool ring_empty(struct ring *);

#endif // __LIBAPTH_UTILS_RING_H
