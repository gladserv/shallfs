#ifndef _SHALL_SUPER_H_
#define _SHALL_SUPER_H_

/* update superblock from current data; note that the caller must
 * hold the superblock info mutex! */

extern struct shall_fsinfo * shall_fs_list;
extern struct mutex shall_fs_mutex;

#endif /* _SHALL_SUPER_H_ */
