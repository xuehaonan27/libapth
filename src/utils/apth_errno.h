#ifndef __LIBAPTH_UTILS_APTH_ERRNO_H
#define __LIBAPTH_UTILS_APTH_ERRNO_H

#include <errno.h>

#define apth_shield                                           \
    for (int apth_errno_storage = errno, apth_errno_flag = 1; \
         apth_errno_flag;                                     \
         errno = apth_errno_storage, apth_errno_flag = 0)

/* return plus setting an errno value */
#ifdef APTH_DEBUG
#define apth_error(return_val, errno_val)                                  \
    (errno = (errno_val),                                                  \
     apth_debug4("return 0x%lx with errno %d(\"%s\")",                     \
                 (unsigned long)(return_val), (errno), strerror((errno))), \
     (return_val))
#else
#define apth_error(return_val, errno_val) \
    (errno = (errno_val), (return_val))
#endif

#endif // __LIBAPTH_UTILS_APTH_ERRNO_H
