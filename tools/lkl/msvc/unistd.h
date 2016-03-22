/* hack header to compile fixdep etc */

#ifndef __MSVC_HACK_UNISTD_H
#define __MSVC_HACK_UNISTD_H

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h> /* _MAX_PATH */
#include <io.h> 

#define PATH_MAX _MAX_PATH

#define close _close
#define read _read

typedef int ssize_t;

#endif /* __MSVC_HACK_UNISTD_H */
