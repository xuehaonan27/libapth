#include "apth.h"
#include "internal/types.h"
#include "utils/apth_errno.h"
#include <string.h>

int apth_getname_np(apth_t th, char name[], size_t size)
{
    if (!APTH_IS_VALID(th))
        return apth_error(ESRCH, ESRCH);
    if (name == NULL)
        return apth_error(EINVAL, EINVAL);

    size_t name_len = strlen(th->name);
    if (size <= name_len)
        return apth_error(ERANGE, ERANGE);

    memcpy(name, th->name, name_len);
    name[name_len + 1] = '\0';
    return 0;
}