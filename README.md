# Libapth: userspace thread library for modern multi-CPU environment

+ A userspace thread (M:N) library. Userspace threads in `libapth` are called `apth`s.
+ Provides API mimicking GNU NPTL (pthread). Theoratically, just open your favorite editor, replace each `pthread_` with `apth_`, `PTHREAD_` with `APTH_`, and of course `pthread.h` with `apth.h`, and the program will still compile and run, with an improved performance. Uh, of course, **THEORATICALLY**. You will never know how much dirty things' there, especially when you are hacking LIBC.
+ **NOT A COROUTINE OR ASYNCHRONOUS I/O LIBRARY**. If you are looking for a coroutine or asynchronous I/O library, `libapth` might not be your choice.
+ Aiming at transforming `pthread` based program into a user space scheduled one with improved performance. That is to say, `libapth` will cover up I/O wait time as much as it could. But that could hardly to be said as "asynchronous I/O", since `libapth` will still provide a synchronous I/O illusion to program depending on it. 
+ Besides, since all threads are scheduled in user space, a lot of kernel-user context switches are mitigated. To achieve this aim, workers (OS-threads, meaning `pthread`s on POSIX platform) that carries workloads (`apth`s) are 1 to 1 bound to CPU cores.
+ There's plan to add time slicing option to `libapth`.

## Notice
Since `libapth` provides improved performance mainly by modifying how workloads are scheduled, so POSIX thread APIs related to scheduling policy, parameters and resource competition are actually not functional.
+ `apth_attr_getscope`, `apth_attr_setscope`
+ `apth_attr_getschedpolicy`, `apth_attr_setschedpolicy`
+ `apth_attr_getschedparam`, `apth_attr_setschedparam`
They are here just to make your code compiles. Besides, `libapth` will provide an extension for programmer to configure the scheduler.

## Conditional Compilation
NOTE: only for developers. You do not want to compile the library with these debug options and gain a lot of garbage STDERR messages.

0. APTH_DEBUG: Control whether debug is enabled
1. APTH_DEBUG_LLL: low level lock debug
    1.1: APTH_DEBUG_LLL_USING_FPRINTF
2. APTH_DEBUG_HOLD_INITIALIZER_PTHREAD: hold initializer pthread by a dead loop
3. APTH_DEBUG_SYSCALL_INIT_DBG: initializing syscall hook debug

## TODO
1. Hybrid scheduling (with I/O bounded workloads spawned as `apth`s and compute bounded ones as `pthread`s, occupying a whole worker)
2. Check return values
3. Check return values of push_apth_to, pop_apth_from, head_apth_of
4. Special treat main thread, when create, exit, cancel, detach, join, ...
5. Cancellation points (see pthread(7))
6. Can write tests according to manual pages of pthread
7. Unify format of debug messages
8. For any `apth_t` passed in, check its validity first.
9. Make all internal functions private (static)
10. Using APTH_INTERNAL and APTH_API for controlling visibility.
11. A macro for directly writing main apth.
12. Current event manager goes over each event of every waiting apth, which is of low efficiency.
13. Event manager should use `poll` or `epoll` instead of old and slow `select`.
14. Check all passed in arguments to API functions (e.g. `th`) is valid.
15. Hook STDIO (e.g. printf, fprintf...) *
16. Better handling of cancelling (e.g. now we have way too many fields for cancellation)
17. The use of `apth_error` should be regular.
18. Scheduler work stealing. Should consider the CPU affinity of the apth,
and the type of the apth (e.g. for GC Worker threads, it's better to distribute
them evenly across all schedulers, accompanying mutator threads)
19. apth_sigmask test
20. Self-make allocator
21. If there's too many terminated apths, scheduler should schedule apth
that's waiting to join others as quick as possible