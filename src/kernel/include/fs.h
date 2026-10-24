#ifndef FS_H_MODULE_H
#define FS_H_MODULE_H

/* Module interface header (M4). Cross-module code includes this, not internals. */

#include "fs/fs.h"
#include "fs/fat16.h"
#include "fs/pipe.h"
#include "fs/ramfs.h"

/* selftests */
void fs_backend_selftest(void);

#endif
