# Libapth: userspace thread library for modern multi-CPU environment

## Conditional Compilation
0. APTH_DEBUG: Control whether debug is enabled
1. APTH_DEBUG_LLL: low level lock debug
    1.1: APTH_DEBUG_LLL_USING_FPRINTF
2. APTH_DEBUG_HOLD_INITIALIZER_PTHREAD: hold initializer pthread by a dead loop
3. APTH_DEBUG_SYSCALL_INIT_DBG: initializing syscall hook debug

## TODO
1. Hybrid scheduling
2. Check return values
3. Check return values of push_apth_to, pop_apth_from, head_apth_of
4. Special treat main thread, when create, exit, cancel, detach, join, ...
5. Cancellation points (see pthread(7))
6. Can write tests according to manual pages of pthread
7. Unify format of debug messages
8. For any passed in apth_t, check its validity first.
9. Make all internal functions private (static)
10. Using APTH_INTERNAL and APTH_API for controlling visibility.
11. A macro for directly writing main apth.
12. Current event manager goes over each event of every waiting apth, which is of low efficiency.
13. Event manager should use `poll` or `epoll` instead of old and slow `select`.
14. Check all passed in arguments to API functions (e.g. `th`) is valid.
15. Hook STDIO (e.g. printf, fprintf...)
16. Better handling of cancelling (e.g. now we have way too many fields for cancellation)
17. The use of `apth_error` should be regular.