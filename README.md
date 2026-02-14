# Libapth: userspace thread library for modern multi-CPU environment

## TODO
1. Hybrid scheduling
2. Check return values
3. Check return values of push_apth_to, pop_apth_from, head_apth_of
4. Special treat main thread, when create, exit, cancel, detach, join, ...
5. Cancellation points (see pthread(7))
6. Can write tests according to manual pages of pthread
7. Unify format of debug messages
8. For any passed in apth_t, check its validity first.
