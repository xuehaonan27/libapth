# LIBAPTH: A Userspace Thread Library

LIBAPTH is a userspace thread library designed to minimize kernel-userspace
context switches and maximize I/O efficiency. It creates multiple user-space
scheduled threads (apths) on top of several pthread-based workers.

Applications built with LIBAPTH can achieve better performance in I/O-bound
workloads by reducing unnecessary context switches and keeping the CPU busy
with useful work instead of waiting for I/O operations to complete.

**Design Goals**:
1. Minimize kernel-userspace context switches
2. Maximize I/O efficiency
3. Provide pthread-compatible API

## Compile and Install
```shell
make all
sudo make install
# Use LIBAPTH by including <apth.h> instead of <pthread.h>
```

## Quick Start
The only thing different when using LIBAPTH is main function:
```C
// Configure total workers here. Gain better performance if the worker number
// is less than or equal to available CPU cores.
APTH_CONFIG(cfg, cfg->workers = 4;)

// Main entry. `argc` and `argv` could be changed to other names you like.
APTH_MAIN_BEGIN(argc, argv)
{
    // Write your main function
}
APTH_MAIN_END // 
```


## Key Points
1. LIBAPTH is a userspace thread library. Userspace threads in LIBAPTH are 
called `apth`s. LIBAPTH is **NOT** a coroutine or asynchronous I/O library.
+ Provides API mimicking GNU NPTL (Pthread). Theoratically, just open your
favorite editor, replace each `pthread_` with `apth_`, `PTHREAD_` with `APTH_`,
 and of course `pthread.h` with `apth.h`, and the program will still compile 
and run, with an improved performance. Of course, **THEORATICALLY**. You will 
never know how much dirty things' there, especially when you are hacking LIBC.
What described above is what I am pursuing to achieve and is not any kind of
warranty.
+ Aiming at transforming Pthread based program into a user space scheduled one 
with improved performance. LIBAPTH covers up I/O wait time as much as it can.
+ All `apth`s are scheduled in userspace, so a lot of kernel-user context 
switches are mitigated. Workers (kernel threads, which are Pthreads on POSIX 
platform) that carry workloads (`apth`s) are 1:1 bound to CPU cores.

## Limitations
1. Currently only supports x86-64 Linux platform. And LIBAPTH semantics come 
most from GNU LIBC. BSD Unix and MUSL semantics not considered.
2. No time slicing capability. Currently LIBAPTH provides a very light weight 
API  `apth_yield_optionally` to detect whether it's time to yield. It's the 
programmer's duty to insert as much as possible into the program. Compiler 
interpolation method for inserting such checkpoints into the binary might be
implemented in the future. Latest Intel processors might have UINTR (userspace 
interrupt) mechanism, which is useful for implementing user space time slicing 
mechanism

## Bad APIs
Since `libapth` provides improved performance mainly by changing how workloads 
are scheduled, so POSIX thread APIs related to scheduling policy, parameters 
and resource competition are actually not functional. Such APIs are listed here:
+ `apth_attr_getscope`, `apth_attr_setscope`
+ `apth_attr_getschedpolicy`, `apth_attr_setschedpolicy`
+ `apth_attr_getschedparam`, `apth_attr_setschedparam`
They are here just to make your code compiles. Besides, `libapth` will provide 
an extension for programmer to configure the scheduler.

## Conditional Compilation
1. APTH_HOLD_INITIALIZER_PTHREAD: hold initializer Pthread waiting to join
the first scheduler of LIBAPTH. Enabled by default.
2. APTH_STACKGROWTH: indicating the stack growth direction. Default to -1,
indicating the stack grows downwards, which is expected on most platforms. If
that's not the case on your platform, give this flag a positive value.

## Developer Conditional Compilation Flags
**NOTE**: only for developers.
1. APTH_DEBUG: Control whether debug is enabled
2. APTH_DEBUG_LLL: low level lock debug
    2.1: APTH_DEBUG_LLL_USING_FPRINTF

## TODO List
1. Hybrid scheduling (with I/O bounded workloads spawned as `apth`s and 
computation bounded ones as `pthread`s, occupying a whole worker)
2. Check return values.
3. Cancellation points (see pthread(7))
4. Can write tests according to manual pages of pthread
5. For any `apth_t` passed in, check its validity first.
6. Check all passed in arguments to API functions (e.g. `th`) is valid.
7. Hook stream I/O (e.g. printf, fprintf...)
8. Better cancellation (now there's too many fields for cancellation)
9. Should consider the type of the apth (e.g. for GC Worker threads in JVM, it 
is better to distribute them evenly across all schedulers, accompanying other 
mutator threads)
10. Memory allocator designed delibratedly.
11. On Linux platform, mechanisms like signalfd, eventfd should also be 
considered and hooked.

## Memory Allocator
Structures that needs allocation:
1. Stack. Should treated specially.
2. `struct __apth_main_args *__margs__= malloc(sizeof(struct __apth_main_args));`. But this is only one.
3. `iattr->cpuset`. `cpu_set_t`.
4. `malloc(sizeof(struct apth_cleanup_st))`. This struct is currently 3 pointers, meaning 3 words = 12 bytes (32 bits platform) or 24 bytes (64 bits platform). It is aligned to 8 bytes! Should be allocated in per-scheduler TLAB.
5. `tiov = (struct iovec *)malloc(tiovcnt)` in scatter_gather I/O. But could it be optimized to stack allocation?
6. `struct apth_epoll_waiter *w = (struct apth_epoll_waiter *)malloc(sizeof(*w));`
 . They should be allocated in per-scheduler TLAB! So the global memory pool should instead maintain a per-pthread memory block!
7. `sched = (apth_sched_t)malloc(sizeof(struct apth_sched_st))`. This is allocated only once per pthread. So could be allocated in global pool.
8. `t = (apth_t)malloc(sizeof(struct apth_st))`. TCB should be merged with CTX, and fill a 4 KiB page.
10. `(apth_worker_arg_t)malloc(sizeof(struct apth_worker_pthread_arg))`. Is allocated only once per pthread.
11. `(rv = (char *)malloc(n + 1))`. This is used in `apth_string`. Could we consider allocate on stack? Since LIBAPTH currently do not support time slicing, so we could just do that on scheduler's stack?