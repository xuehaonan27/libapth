// Architecture / Platform / Toolchain dependent
#ifndef __LIBAPTH_UTILS_ARCHPLATTOOLD_H
#define __LIBAPTH_UTILS_ARCHPLATTOOLD_H

#define apth_expect(x, val) __builtin_expect(!!(x), (val))
#define apth_likely(x) __builtin_expect(!!(x), 1)
#define apth_unlikely(x) __builtin_expect(!!(x), 0)

#endif // __LIBAPTH_UTILS_ARCHPLATTOOLD_H
