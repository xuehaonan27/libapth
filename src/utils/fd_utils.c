#include "internal_types.h"
#include "internal_funcs.h"

void apth_util_fds_merge(int nfd,
                         fd_set *ifds1, fd_set *ofds1,
                         fd_set *ifds2, fd_set *ofds2,
                         fd_set *ifds3, fd_set *ofds3)
{
    int s;

    for (s = 0; s < nfd; s++)
    {
        if (ifds1 != NULL)
            if (FD_ISSET(s, ifds1))
                FD_SET(s, ofds1);
        if (ifds2 != NULL)
            if (FD_ISSET(s, ifds2))
                FD_SET(s, ofds2);
        if (ifds3 != NULL)
            if (FD_ISSET(s, ifds3))
                FD_SET(s, ofds3);
    }
    return;
}

// test whether fds in the input fd sets occurred in the output fds
bool apth_util_fds_test(int nfd,
                        fd_set *ifds1, fd_set *ofds1,
                        fd_set *ifds2, fd_set *ofds2,
                        fd_set *ifds3, fd_set *ofds3)
{
    int s;

    for (s = 0; s < nfd; s++)
    {
        if (ifds1 != NULL)
            if (FD_ISSET(s, ifds1) && FD_ISSET(s, ofds1))
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
int apth_util_fds_select(int nfd,
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
