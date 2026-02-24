#include "internal_types.h"
#include "internal_funcs.h"
#include "utils/apth_errno.h"
#include <fcntl.h>

APTH_INTERNAL bool apth_util_fd_valid(int fd)
{
    if (fd < 0 || fd >= FD_SETSIZE)
        return false;
    if (fcntl(fd, F_GETFL) == -1 && errno == EBADF)
        return false;
    return true;
}

APTH_INTERNAL void apth_util_fds_merge(int nfd,
                                       fd_set *ifds1, fd_set *ofds1,
                                       fd_set *ifds2, fd_set *ofds2,
                                       fd_set *ifds3, fd_set *ofds3)
{
    int s;

    for (s = 0; s < nfd; s++)
    {
        if (ifds1 != NULL && FD_ISSET(s, ifds1))
            FD_SET(s, ofds1);
        if (ifds2 != NULL && FD_ISSET(s, ifds2))
            FD_SET(s, ofds2);
        if (ifds3 != NULL && FD_ISSET(s, ifds3))
            FD_SET(s, ofds3);
    }
    return;
}

// test whether fds in the input fd sets occurred in the output fds
APTH_INTERNAL bool apth_util_fds_test(int nfd,
                                      fd_set *ifds1, fd_set *ofds1,
                                      fd_set *ifds2, fd_set *ofds2,
                                      fd_set *ifds3, fd_set *ofds3)
{
    int s;

    for (s = 0; s < nfd; s++)
    {
        if (ifds1 != NULL && FD_ISSET(s, ifds1) && FD_ISSET(s, ofds1))
                return true;
        if (ifds2 != NULL)
            if (FD_ISSET(s, ifds2) && FD_ISSET(s, ofds2))
                return true;
        if (ifds3 != NULL)
            if (FD_ISSET(s, ifds3) && FD_ISSET(s, ofds3))
                return true;
    }
    return false;
}

// Clear fds in input fd sets if not occurred in output fd sets and return
// number of remaining input fds. This number uses BSD select(2) semantics: a
// fd in two set counts twice!
APTH_INTERNAL int apth_util_fds_select(int nfd,
                                       fd_set *ifds1, fd_set *ofds1,
                                       fd_set *ifds2, fd_set *ofds2,
                                       fd_set *ifds3, fd_set *ofds3)
{
    int s;
    int n;

    n = 0;
    for (s = 0; s < nfd; s++)
    {
        if (ifds1 != NULL && FD_ISSET(s, ifds1))
        {
            if (!FD_ISSET(s, ofds1))
                FD_CLR(s, ifds1);
            else
                n++;
        }
        if (ifds2 != NULL && FD_ISSET(s, ifds2))
        {
            if (!FD_ISSET(s, ofds2))
                FD_CLR(s, ifds2);
            else
                n++;
        }
        if (ifds3 != NULL && FD_ISSET(s, ifds3))
        {
            if (!FD_ISSET(s, ofds3))
                FD_CLR(s, ifds3);
            else
                n++;
        }
    }
    return n;
}

// Switch a filedescriptor's I/O mode
APTH_INTERNAL int apth_fdmode(int fd, int newmode)
{
    int fdmode;
    int oldmode;

    // Retrieve old mode (usually a very cheap operation)
    fdmode = fcntl(fd, F_GETFL, NULL);
    if (fdmode == -1)
        oldmode = APTH_FDMODE_ERROR;
    else if (fdmode & APTH_O_NONBLOCKING)
        oldmode = APTH_FDMODE_NONBLOCK;
    else
        oldmode = APTH_FDMODE_BLOCK;

    // Set new mode (usually a more expensive operation)
    if (oldmode == APTH_FDMODE_BLOCK && newmode == APTH_FDMODE_NONBLOCK)
        fcntl(fd, F_SETFL, (fdmode | APTH_O_NONBLOCKING));
    if (oldmode == APTH_FDMODE_NONBLOCK && newmode == APTH_FDMODE_BLOCK)
        fcntl(fd, F_SETFL, (fdmode & ~(APTH_O_NONBLOCKING)));

    // Return old mode
    return oldmode;
}