#ifndef _LIBAPTH_UTILS_ATOMIC_WRAPPER_H
#define _LIBAPTH_UTILS_ATOMIC_WRAPPER_H

#include <stdatomic.h>
#include "archplattoold.h"

#define atomic_thread_fence_acquire() atomic_thread_fence(__ATOMIC_ACQUIRE)
#define atomic_thread_fence_release() atomic_thread_fence(__ATOMIC_RELEASE)
#define atomic_thread_fence_seq_cst() atomic_thread_fence(__ATOMIC_SEQ_CST)

#define atomic_full_barrier() __asm("" ::: "memory")
#define atomic_read_barrier() atomic_full_barrier()
#define atomic_write_barrier() atomic_full_barrier()

#define atomic_load_relaxed(mem) atomic_load_explicit((mem), __ATOMIC_RELAXED)
#define atomic_load_acquire(mem) atomic_load_explicit((mem), __ATOMIC_ACQUIRE)
#define atomic_load_seqcst(mem) atomic_load_explicit((mem), __ATOMIC_SEQ_CST)

#define atomic_store_relaxed(mem, val) \
    atomic_store_explicit((mem), (val), __ATOMIC_RELAXED)
#define atomic_store_release(mem, val) \
    atomic_store_explicit((mem), (val), __ATOMIC_RELEASE)

/* On failure, this CAS has memory_order_relaxed semantics. */
#define atomic_compare_exchange_weak_relaxed(mem, expected, desired) \
    atomic_compare_exchange_weak_explicit(                           \
        (mem),                                                       \
        (expected),                                                  \
        (desired),                                                   \
        __ATOMIC_RELAXED,                                            \
        __ATOMIC_RELAXED)
#define atomic_compare_exchange_weak_acquire(mem, expected, desired) \
    atomic_compare_exchange_weak_explicit(                           \
        (mem),                                                       \
        (expected),                                                  \
        (desired),                                                   \
        __ATOMIC_ACQUIRE,                                            \
        __ATOMIC_RELAXED)
#define atomic_compare_exchange_weak_release(mem, expected, desired) \
    atomic_compare_exchange_weak_explicit(                           \
        (mem),                                                       \
        (expected),                                                  \
        (desired),                                                   \
        __ATOMIC_RELEASE,                                            \
        __ATOMIC_RELAXED)

#define atomic_compare_exchange_relaxed(mem, expected, desired) \
    atomic_compare_exchange_strong_explicit(                    \
        (mem),                                                  \
        (expected),                                             \
        (desired),                                              \
        __ATOMIC_RELAXED,                                       \
        __ATOMIC_RELAXED)
#define atomic_compare_exchange_acquire(mem, expected, desired) \
    atomic_compare_exchange_strong_explicit(                    \
        (mem),                                                  \
        (expected),                                             \
        (desired),                                              \
        __ATOMIC_ACQUIRE,                                       \
        __ATOMIC_RELAXED)
#define atomic_compare_exchange_release(mem, expected, desired) \
    atomic_compare_exchange_strong_explicit(                    \
        (mem),                                                  \
        (expected),                                             \
        (desired),                                              \
        __ATOMIC_RELEASE,                                       \
        __ATOMIC_RELAXED)

#define atomic_exchange_relaxed(mem, desired) \
    atomic_exchange_explicit((mem), (desired), __ATOMIC_RELAXED)
#define atomic_exchange_acquire(mem, desired) \
    atomic_exchange_explicit((mem), (desired), __ATOMIC_ACQUIRE)
#define atomic_exchange_release(mem, desired) \
    atomic_exchange_explicit((mem), (desired), __ATOMIC_RELEASE)

#define atomic_fetch_add_relaxed(mem, operand) \
    atomic_fetch_add_explicit((mem), (operand), __ATOMIC_RELAXED)
#define atomic_fetch_add_acquire(mem, operand) \
    atomic_fetch_add_explicit((mem), (operand), __ATOMIC_ACQUIRE)
#define atomic_fetch_add_release(mem, operand) \
    atomic_fetch_add_explicit((mem), (operand), __ATOMIC_RELEASE)
#define atomic_fetch_add_acq_rel(mem, operand) \
    atomic_fetch_add_explicit((mem), (operand), __ATOMIC_ACQ_REL)

#define atomic_fetch_and_relaxed(mem, operand) \
    atomic_fetch_and_explicit((mem), (operand), __ATOMIC_RELAXED)
#define atomic_fetch_and_acquire(mem, operand) \
    atomic_fetch_and_explicit((mem), (operand), __ATOMIC_ACQUIRE)
#define atomic_fetch_and_release(mem, operand) \
    atomic_fetch_and_explicit((mem), (operand), __ATOMIC_RELEASE)

#define atomic_fetch_or_relaxed(mem, operand) \
    atomic_fetch_or_explicit((mem), (operand), __ATOMIC_RELAXED)
#define atomic_fetch_or_acquire(mem, operand) \
    atomic_fetch_or_explicit((mem), (operand), __ATOMIC_ACQUIRE)
#define atomic_fetch_or_release(mem, operand) \
    atomic_fetch_or_explicit((mem), (operand), __ATOMIC_RELEASE)

#define atomic_fetch_xor_release(mem, operand) \
    atomic_fetch_xor_explicit((mem), (operand), __ATOMIC_RELEASE)

#define atomic_compare_and_exchange_val_acq(mem, newval, oldval)      \
    ({                                                                \
        typeof(*(mem)) __tmp = (oldval);                              \
        atomic_compare_exchange_acquire(mem, (void *)&__tmp, newval); \
        __tmp;                                                        \
    })
#define atomic_compare_and_exchange_val_rel(mem, newval, oldval)      \
    ({                                                                \
        typeof(*(mem)) __tmp = (oldval);                              \
        atomic_compare_exchange_release(mem, (void *)&__tmp, newval); \
        __tmp;                                                        \
    })
// Returns 0 if cmpxchg succeeds, true(!0) if fails.
#define atomic_compare_and_exchange_bool_acq(mem, newval, oldval)      \
    ({                                                                 \
        typeof(*(mem)) __tmp = (oldval);                               \
        !atomic_compare_exchange_acquire(mem, &__tmp, newval); \
    })

#define atomic_max(mem, value)                          \
    do                                                  \
    {                                                   \
        typeof(*(mem)) __tmp_oldval;                    \
        typeof(mem) __tmp_memp = (mem);                 \
        typeof(*(mem)) __tmp_value = (value);           \
        do                                              \
        {                                               \
            __tmp_oldval = *__tmp_memp;                 \
            if (__tmp_oldval >= __tmp_value)            \
                break;                                  \
        } while (apth_expect(                           \
            atomic_compare_and_exchange_bool_acq(       \
                __tmp_memp, __tmp_value, __tmp_oldval), \
            0));                                        \
    } while (0)

#define atomic_decrement_if_positive(mem)                    \
    ({                                                       \
        typeof(*(mem)) __tmp_oldval;                         \
        typeof(mem) __tmp_memp = (mem);                      \
        do                                                   \
        {                                                    \
            __tmp_oldval = *__tmp_memp;                      \
            if (apth_unlikely(__tmp_oldval <= 0))            \
                break;                                       \
        } while (apth_expect(                                \
            atomic_compare_and_exchange_bool_acq(            \
                __tmp_memp, __tmp_oldval - 1, __tmp_oldval), \
            0));                                             \
        __tmp_oldval;                                        \
    })

#endif // _LIBAPTH_UTILS_ATOMIC_WRAPPER_H
