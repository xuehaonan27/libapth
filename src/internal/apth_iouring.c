#include "apth_iouring.h"

#ifdef APTH_USE_IOURING

#include "utils/debug.h"
#include <errno.h>
#include <string.h>

// One-time probe result: -1 = not probed, 0 = unavailable, 1 = available
static int iouring_probed = -1;
static bool iouring_fast_poll = false;

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
            io_uring_queue_exit(&ring);
            iouring_probed = 1;
            apth_debug("io_uring: available (fast_poll=%d)", iouring_fast_poll);
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

    int ret = io_uring_queue_init(queue_depth, &ctx->ring, 0);
    if (ret < 0)
    {
        apth_debug("io_uring_queue_init failed: %d", ret);
        return ret;
    }

    ctx->initialized = true;
    apth_debug("io_uring: ring initialized (depth=%d)", queue_depth);
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
