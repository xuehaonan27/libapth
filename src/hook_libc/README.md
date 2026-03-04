# Hook LIBC functions

## NOTE for header including and namespace protection
Note that we should always include minimal header for the purpose, in a single compilation unit.

For example, if we need `struct timespec`, we should include `<bits/types/struct_timespec.h>` and avoid include a HUGE header (like `<unistd.h>`, `stdlib.h` or something like that). Because we are hooking LIBC, providing exactly same function prototypes as LIBC, compiler will be unhappy if we happen to redefine a name in a compilation unit. By including minimal header in a compilation unit, we could avoid this problem as much as possible.
