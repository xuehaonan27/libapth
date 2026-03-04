The project currently open in the workspace is a userspace thread library called LIBAPTH. Its core idea is to generate multiple user space scheduled `apth`s (userspace-scheduled threads) on top of several pthread-based workers. Each worker can run multiple `apth` workloads, while each `apth` must still provide the upper-layer application with the abstraction of an independent thread as much as possible. At the same time, it aims to reduce kernel-userspace context switches and improve I/O efficiency.

Design goals of this library:
1. Minimize kernel-userspace context switches as much as possible, hence the library seeks to avoid unnecessary system calls wherever possible.
2. Maximize I/O efficiency, hoping that when some `apth`s are waiting for I/O, the scheduler can dispatch other workloads to the CPU, allowing the CPU to perform useful tasks instead of busy-waiting. This enhances CPU utilization while still providing the original I/O semantics and abstraction to the upper-layer application. (Therefore, this library is not an Asynchronous I/O library.)
3. Provide a Pthread Compatible API.

Structure of this library:
1. The only header file exposed externally is `src/apth.h`.
2. Most type and function definitions are located in `src/internal_types.h` and `src/internal_funcs.h`.
3. Each pthread worker (defined in `src/internal/apth_worker.c`) runs a `scheduler_routine` (its behavior described in `src/internal/apth_sched.c`). Within the scheduler, there are multiple `apth_thqueue_t` (described in `src/internal/apth_thqueue.c`) that hold `apth`s in different states. The `scheduler_routine` continuously retrieves a new apth from the ready queue, passes the context to it, and waits for it to return the context to the scheduler (by actively calling `apth_yield`, terminating, being canceled, or waiting for an event). The scheduler determines the current state of the thread and reinserts it into the appropriate queue.
4. Signal handling is mostly described in `src/internal/apth_signal.c`.
5. Event handling within LIBAPTH is described in `src/internal/apth_event.c`. The scheduler runs `apth_sched_eventmanager` (defined in `src/internal/apth_event.c`) to check whether any `apth` in the scheduler's waiting queue has had its awaited event satisfied, thus meeting the condition to proceed. If so, that `apth` is moved into the waked queue.
6. Various files under `src/core/` and `src/attr/` provide API functions highly similar to those in pthread.
7. `src/utils/` contains some utility functions.

To ensure the correctness of this library's implementation and to improve I/O efficiency, a module composed of several files under `src/hook_libc/` hooks a series of libc functions. These broadly fall into the following categories:
+ **process**: used to align with the threading model of LIBAPTH.
+ **pthread**: This part is currently hooked; LIBAPTH merely retrieves its symbols via `dlsym`.
+ **signal**: used to integrate with LIBAPTH's built-in signal handling.
+ **socket**, **lowlevel_io**: For I/O operations in these two categories, the hooking modifies their behavior. Instead of potentially blocking as originally intended, they now place an event indicating waiting for that I/O event if the condition is not met (e.g., if a file descriptor is temporarily unwritable). They then immediately yield control, quickly switching back to the scheduler. This allows the scheduler to dispatch the next runnable `apth` to the CPU core, thereby improving CPU utilization.

When running a program, preloading this library with `LD_PRELOAD` allows certain libc function calls made by the application to be redirected to this library.