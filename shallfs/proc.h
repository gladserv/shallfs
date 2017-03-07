#ifndef _SHALL_PROC_H
#define _SHALL_PROC_H

extern struct file_operations shall_proc_mounted;

extern struct file_operations shall_proc_info;
extern struct file_operations shall_proc_blog;
#ifdef CONFIG_SHALL_FS_DEBUG
extern struct file_operations shall_proc_hlog;
#endif
extern struct file_operations shall_proc_ctrl;

void shall_notify_umount(struct shall_fsinfo *);
#endif /* _SHALL_PROC_H */
