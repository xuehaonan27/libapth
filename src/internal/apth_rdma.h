#ifndef __LIBAPTH_INTERNAL_APTH_RDMA_H
#define __LIBAPTH_INTERNAL_APTH_RDMA_H

#ifdef APTH_USE_RDMA

/*
 * RDMA Completion Poller for LIBAPTH
 *
 * Provides a dedicated polling thread that monitors ibverbs completion
 * queues (CQs) and wakes apths when their RDMA operations complete.
 *
 * Unlike FD I/O (which uses epoll/io_uring and involves syscalls),
 * RDMA completion polling is pure userspace: ibv_poll_cq() is a
 * memory-mapped read, costing ~10ns per poll with no kernel transition.
 *
 * Usage:
 *   1. Application calls apth_rdma_register_cq() for each CQ
 *   2. Thread posts RDMA work request via ibv_post_send()
 *   3. Thread calls apth_rdma_wait(cq, wr_id, &wc) to yield until done
 *   4. Poller detects completion, marks event, wakes scheduler
 *   5. Thread resumes with completion data in wc
 */

#include "apth.h"
#include "internal/forward_declare.h"
#include "utils/archplattoold.h"
#include "utils/lll.h"
#include <stdbool.h>
#include <stdint.h>

/* Opaque types for ibverbs structures.
 * Users must include <infiniband/verbs.h> themselves. */
struct ibv_cq;
struct ibv_wc;

/* Maximum registered CQs, concurrent RDMA waiters, and stash size. */
#define APTH_RDMA_MAX_CQS     64
#define APTH_RDMA_MAX_WAITERS  4096
#define APTH_RDMA_STASH_SIZE   256

/* A pending RDMA wait request. */
struct apth_rdma_waiter
{
    apth_event_t ev;         /* Event to mark OCCURRED */
    apth_t th;               /* Owning thread */
    apth_sched_t sched;      /* Scheduler to wake */
    struct ibv_cq *cq;       /* CQ this waiter is waiting on */
    uint64_t wr_id;          /* Work request ID to match */
    struct ibv_wc *wc_out;   /* Where to store the completion */
    bool active;             /* Slot in use */
};

/* A CQE polled by one thread's fast path that didn't match its wr_id.
 * Stashed here so the rightful owner can retrieve it later. */
struct apth_rdma_stashed_cqe
{
    struct ibv_cq *cq;
    struct ibv_wc wc;
};

/* Global RDMA poller state. */
struct apth_rdma_poller
{
    pthread_t thread;
    _Atomic(bool) running;

    /* Registered completion queues. */
    struct {
        struct ibv_cq *cq;
        bool active;
    } cqs[APTH_RDMA_MAX_CQS];
    int cq_count;
    lll_internal_t cq_lock;

    /* Waiter table: indexed for fast lookup.
     * New waiters are appended; completed ones are marked inactive. */
    struct apth_rdma_waiter waiters[APTH_RDMA_MAX_WAITERS];
    _Atomic(int) waiter_count;
    lll_internal_t waiter_lock;

    /* Stash for CQEs polled by fast-path callers that didn't match.
     * Protected by waiter_lock (shared with waiter table). */
    struct apth_rdma_stashed_cqe stash[APTH_RDMA_STASH_SIZE];
    int stash_count;
};

extern struct apth_rdma_poller GLOBAL_RDMA_POLLER;

/* Lifecycle. */
APTH_INTERNAL int apth_rdma_poller_start(void);
APTH_INTERNAL void apth_rdma_poller_stop(void);

/* Register/unregister a CQ with the poller.
 * Must be called before any apth_rdma_wait() on that CQ. */
APTH_INTERNAL int apth_rdma_register_cq(struct ibv_cq *cq);
APTH_INTERNAL void apth_rdma_unregister_cq(struct ibv_cq *cq);

/* Wait for an RDMA completion.
 *
 * Fast path: polls the CQ once (userspace memory read, ~10ns).
 * If the completion is already available, returns immediately with
 * zero context switches.
 *
 * Slow path: registers with the poller, yields to the scheduler,
 * and resumes when the poller detects the completion.
 *
 * Returns 0 on success (wc filled), -1 on error. */
APTH_INTERNAL int apth_rdma_wait(struct ibv_cq *cq, uint64_t wr_id,
                                  struct ibv_wc *wc);

/* Wait for a batch of RDMA completions.
 * Yields once, resumes when all wr_ids have completed.
 * Returns 0 on success, -1 if any completion failed. */
APTH_INTERNAL int apth_rdma_wait_batch(struct ibv_cq *cq,
                                        uint64_t *wr_ids, int count,
                                        struct ibv_wc *wcs);

#endif /* APTH_USE_RDMA */

#endif /* __LIBAPTH_INTERNAL_APTH_RDMA_H */
