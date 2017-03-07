/* modify an unmounted shallfs device */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <unistd.h>
#include <endian.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "shallfs-common.h"
#include <shallfs/operation.h>
#include <shallfs/device.h>

// 1. read all superblocks and compare the information for consistency
//    1.1 if the superblock is marked as updating, load the move plan
//        from it and go to step 3
//    1.2 if any recovery required, exit asking to run shallfsck
// 2. calculate an update plan, which may include moving of blocks; also
//    update superblock information.
// 3. if block moving is requested, store the update plan in the reserved
//    superblock area, mark the superblock as updating, and write all
//    superblocks which won't be moving.
// 4. start block move; checkpoint at regular intervals, and make sure that
//    any uncheckpointed move can be repeated without problems, i.e. the
//    source data has not been overwritten.
// 5. once the block move has completed (or if there was no need to move
//    anything) mark the superblock as operational, delete the move plan,
//    and update all superblocks.

