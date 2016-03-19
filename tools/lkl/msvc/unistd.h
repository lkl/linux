/* hack header to compile fixdep etc */

#ifndef __MSVC_HACK_UNISTD_H
#define __MSVC_HACK_UNISTD_H

#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h> /* _MAX_PATH */
#include <io.h> 

#define PATH_MAX _MAX_PATH
#define open _sopen_s
#define close _close


#endif /* __MSVC_HACK_UNISTD_H */
