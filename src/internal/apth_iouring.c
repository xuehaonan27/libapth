#include "apth_iouring.h"

#ifdef APTH_USE_IOURING

#include "utils/debug.h"
#include <errno.h>
#include <string.h>

// One-time probe result: -1 = not probed, 0 = unavailable, 1 = available
static int iouring_probed = -1;
static bool iouring_fast_poll = false;
static bool iouring_has_coop_taskrun = false;
static bool iouring_has_single_issuer = false;

APTH_INTERNAL bool apth_iouring_available(void)
{
    if (iouring_probed < 0)
    {
        struct io_uring_params params;
        memset(&params, 0, sizeof(params));
        struct io_uring ring;
        if (io_uring_queue_init_params(1, &ring, &params) == 0)
        {
            iouring_fast_poll = (params.features & IORING_FEAT_FAST_POLL) != 0;

            // Check for newer kernel features (5.19+)
#ifdef IORING_SETUP_COOP_TASKRUN
            iouring_has_coop_taskrun = true;
#endif
#ifdef IORING_SETUP_SINGLE_ISSUER
            iouring_has_single_issuer = true;
#endif

            io_uring_queue_exit(&ring);
            iouring_probed = 1;
            apth_debug("io_uring: available (fast_poll=%d coop_taskrun=%d single_issuer=%d)",
                       iouring_fast_poll, iouring_has_coop_taskrun, iouring_has_single_issuer);
        }
        else
        {
            iouring_probed = 0;
            apth_debug("io_uring: unavailable (errno=%d), falling back to epoll", errno);
        }
    }
    return iouring_probed == 1;
}

APTH_INTERNAL bool apth_iouring_has_fast_poll(void)
{
    return iouring_fast_poll;
}

APTH_INTERNAL int apth_iouring_init(struct apth_iouring_ctx *ctx, int queue_depth)
{
    ctx->initialized = false;

    // Build optimal flags for a per-scheduler ring:
    // - COOP_TASKRUN (5.19+): Kernel defers task_work to io_uring_enter(),
    //   reducing IPIs and unexpected signal interruptions.
    // - SINGLE_ISSUER (6.0+): Enables internal optimizations knowing only
    //   one thread submits SQEs to this ring.
    unsigned int flags = 0;

#ifdef IORING_SETUP_COOP_TASKRUN
    if (iouring_has_coop_taskrun)
        flags |= IORING_SETUP_COOP_TASKRUN;
#endif
#ifdef IORING_SETUP_SINGLE_ISSUER
    if (iouring_has_single_issuer)
        flags |= IORING_SETUP_SINGLE_ISSUER;
#endif

    int ret = io_uring_queue_init(queue_depth, &ctx->ring, flags);
    if (ret < 0 && flags != 0)
    {
        // Fallback: retry without advanced flags on older kernels
        apth_debug("io_uring: init with flags=0x%x failed (%d), retrying plain", flags, ret);
        ret = io_uring_queue_init(queue_depth, &ctx->ring, 0);
    }
    if (ret < 0)
    {
        apth_debug("io_uring_queue_init failed: %d", ret);
        return ret;
    }

    ctx->initialized = true;
    apth_debug("io_uring: ring initialized (depth=%d, flags=0x%x)", queue_depth, flags);
    return 0;
}

APTH_INTERNAL void apth_iouring_destroy(struct apth_iouring_ctx *ctx)
{
    if (ctx->initialized)
    {
        io_uring_queue_exit(&ctx->ring);
        ctx->initialized = false;
        apth_debug("io_uring: ring destroyed");
    }
}

#endif // APTH_USE_IOURING
