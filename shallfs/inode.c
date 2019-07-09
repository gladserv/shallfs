/* linux/fs/shallfs/inode.c
 *
 *  SPDX-License-Identifier: GPL-2.0-only
 *
 * this file is part of SHALLFS
 *
 * Copyright (c) 2017-2019 Claudio Calvelli <shallfs@gladserv.com>
 *
 *  some code inspired by various Linux filesystems, particularly overlyfs,
 *  which is Copyright (C) 2011 Novell Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program (see the file COPYING in the distribution).
 * If not, see <http://www.gnu.org/licenses/>.
 */


#include <linux/module.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/uio.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/path.h>
#include <linux/time.h>
#include <linux/wait.h>
#include <linux/atomic.h>
#include <linux/file.h>
#include <linux/sched.h>
#include <linux/cred.h>
#include <linux/mount.h>
#include <linux/poll.h>
#include <linux/seq_file.h>
#include <linux/xattr.h>
#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 16, 0)
#include <linux/iversion.h>
#endif
#include <shallfs/operation.h>
#include <shallfs/device.h>
#include "shallfs.h"
#include "log.h"

/* we'll need to see exactly which kernel version for this... XXX */
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 5, 0)
#define lock_inode(i) mutex_lock(i->i_mutex);
#define lock_inode_nested(i, s) mutex_lock_nested(&i->i_mutex, s);
#define unlock_inode(i) mutex_unlock(&i->i_mutex);
#else
#define lock_inode(i) inode_lock(i)
#define lock_inode_nested(i, c) inode_lock_nested(i, c)
#define unlock_inode(i) inode_unlock(i)
#endif

/* we'll need to see exactly which kernel version for this... XXX */
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 5, 0)
#define SHALL_USE_OLD_SYMLINK_CODE 1
#else
#define SHALL_USE_OLD_SYMLINK_CODE 0
#endif

/* we'll need to see exactly which kernel version for this... XXX */
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 5, 0)
#define SHALL_USE_OLD_RENAME_CODE 1
#else
#define SHALL_USE_OLD_RENAME_CODE 0
#endif

/* we'll need to see exactly which kernel version for this... XXX */
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 5, 0)
#define SHALL_USE_OLD_XATTR_CODE 1
#else
#define SHALL_USE_OLD_XATTR_CODE 0
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 18, 0)
typedef struct timespec struct_timespec;
#else
typedef struct timespec64 struct_timespec;
#endif

/* how do we log writes? */
typedef enum {
    shall_log_none,                     /* no logging for this file */
    shall_log_op,                       /* log operation but not data */
    shall_log_hash,                     /* log operation and hash of data */
    shall_log_data,                     /* log operation and data */
} shall_log_mode_t;

/* structure we store in the file */
struct shall_file_data {
	struct file * file;		/* "real" file under fspath */
	struct shall_fsinfo * fi;	/* our filesystem information */
	shall_log_mode_t log_mode;      /* how do we log writes for this file */
	int has_id;			/* has logging started? */
	/* the following is only valid if has_id != 0 */
	unsigned int id;		/* file ID to use when logging */
	int cached_log;			/* is there a cached write log? */
	/* the following is only valid if cached_log != 0 (and has_id != 0) */
	loff_t start;			/* start of cached log region */
	size_t length;			/* length of cached log region */
	loff_t next;			/* byte after end of cached log region
					 * == start + length */
};

static atomic_t last_fileid = ATOMIC_INIT(0);
static struct inode_operations shall_dir_inode_operations;
static struct inode_operations shall_symlink_inode_operations;
static struct inode_operations shall_other_inode_operations;
static struct file_operations shall_file_file_operations;
static struct file_operations shall_dir_file_operations;

struct inode * shall_new_inode(struct super_block *sb, struct dentry *dentry) {
	/* the "dentry" we are given is in the underlying filesystem,
	 * and contains all the information we need */
	struct inode * inode, * orig = dentry->d_inode;
	/* just a wee bit of paranoia */
	if (! orig) return ERR_PTR(-EINVAL);
	/* we don't want to cross mountpoints, things just become weird */
	if (unlikely(d_mountpoint(dentry))) {
		/* however our root inode is a mountpoint, and we must
		 * allow that! */
		struct shall_fsinfo * fi = sb->s_fs_info;
		if (dentry != fi->root_path.dentry)
			return ERR_PTR(-EXDEV);
	}
	/* all looks good, make a new inode */
	inode = new_inode(sb);
	if (! inode) return ERR_PTR(-ENOMEM);
	if (IS_ERR(inode)) return inode;
	inode->i_ino = orig->i_ino;
	inode->i_mode = orig->i_mode;
	inode->i_uid =  orig->i_uid;
	inode->i_gid =  orig->i_gid;
	set_nlink(inode,  orig->i_nlink);
	inode->i_blocks = orig->i_blocks;
	inode->i_size = orig->i_size;
	inode->i_rdev = orig->i_rdev;
	inode->i_atime = orig->i_atime;
	inode->i_mtime = orig->i_mtime;
	inode->i_ctime = orig->i_ctime;
	inode->i_blkbits = orig->i_blkbits;
	inode->i_generation = orig->i_generation;
	inode->i_private = dentry;
	dget(dentry);
	if (S_ISLNK(inode->i_mode)) {
		inode->i_op = &shall_symlink_inode_operations;
		inode->i_fop = NULL;
	} else if (S_ISDIR(inode->i_mode)) {
		inode->i_op = &shall_dir_inode_operations;
		inode->i_fop = &shall_dir_file_operations;
	} else if (S_ISREG(inode->i_mode)) {
		inode->i_op = &shall_other_inode_operations;
		inode->i_fop = &shall_file_file_operations;
	} else {
		inode->i_op = &shall_other_inode_operations;
		inode->i_fop = NULL;
	}
	return inode;
}

static struct dentry * shall_lookup(struct inode *dir, struct dentry *dentry,
				    unsigned int flags)
{
	/* we simply call the lookup function on the underlying filesystem */
	struct inode * inode = NULL;
	struct dentry * lookup, * base = dir->i_private;
	const char * dname = dentry->d_name.name;
	int namelen = dentry->d_name.len;
	/* paranoia */
	if (! base || ! dname) return ERR_PTR(-ENOENT);
	lock_inode(base->d_inode);
	lookup = lookup_one_len(dname, base, namelen);
	unlock_inode(base->d_inode);
	if (IS_ERR(lookup)) return lookup;
	if (lookup->d_inode) {
		inode = shall_new_inode(dir->i_sb, lookup);
		if (IS_ERR(inode)) {
			dput(lookup);
			return ERR_CAST(inode);
		}
	}
	dput(lookup);
	return d_splice_alias(inode, dentry);
}

#if SHALL_USE_OLD_SYMLINK_CODE
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 2, 0)
static void * shall_follow_link(struct dentry *link, struct nameidata *nd) {
	void * p;
#else
static const char * shall_follow_link(struct dentry *link, void ** cookie) {
	const char * p;
#endif
	struct dentry * u_dentry = link->d_inode->i_private;
	struct inode * u_inode;
	if (! u_dentry) return ERR_PTR(-EINVAL);
	u_inode = u_dentry->d_inode;
	if (! (u_inode->i_op && u_inode->i_op->follow_link))
		return ERR_PTR(-ENOSYS);
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 2, 0)
	p = u_inode->i_op->follow_link(u_dentry, nd);
#else
	p = u_inode->i_op->follow_link(u_dentry, cookie);
#endif
	if (IS_ERR_OR_NULL(p)) return p;
	/* what if the target was to /fspath/somefile?  We would like to
	 * rewrite this to /our/mountpoint/somefile - but it's not that easy
	 * and in fact seems to require rewriting the whole path walk, and
	 * that's just to figure out if we did in fact land inside /fspath!
	 * So we hope people don't do that :-) but a future version of this
	 * module may be able to come up with a way to do this */
	return p;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 2, 0)
static void shall_put_link(struct dentry *link, struct nameidata *nd, void *p) {
	struct dentry * u_dentry = link->d_inode->i_private;
#else
static void shall_put_link(struct inode *link, void *p) {
	struct dentry * u_dentry = link->i_private;
#endif
	struct inode * u_inode;
	/* call put_link on the underlying filesystem, if they want it */
	if (! u_dentry) return;
	u_inode = u_dentry->d_inode;
	if (u_inode->i_op && u_inode->i_op->put_link)
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 2, 0)
		u_inode->i_op->put_link(u_dentry, nd, p);
#else
		u_inode->i_op->put_link(u_dentry->d_inode, p);
#endif
}
#else /* SHALL_USE_OLD_SYMLINK_CODE */
const char * shall_get_link(struct dentry * dentry, struct inode * inode,
			    struct delayed_call * callback)
{
	struct dentry * u_dentry;
	struct inode * u_inode;
	if (! dentry) return ERR_PTR(-EINVAL);
	if (! inode) return ERR_PTR(-EINVAL);
	u_dentry = inode->i_private;
	if (! u_dentry) return ERR_PTR(-EINVAL);
	u_inode = u_dentry->d_inode;
	if (! (u_inode->i_op && u_inode->i_op->get_link))
		return ERR_PTR(-ENOSYS);
	return u_inode->i_op->get_link(u_dentry, u_inode, callback);
}
#endif

static int shall_readlink(struct dentry *link, char __user *dest, int len) {
	/* just delegate to the lower fs */
	struct dentry * u_dentry = link->d_inode->i_private;
	struct inode * u_inode;
	if (! u_dentry) return -EINVAL;
	u_inode = u_dentry->d_inode;
	if (! u_inode->i_op || ! u_inode->i_op->readlink)
		return -ENOSYS;
	return u_inode->i_op->readlink(u_dentry, dest, len);
}

/* iterate is what we know as readdir */
static int shall_iterate(struct file *dir, struct dir_context *ctx) {
	/* directory has already been opened, and so we can just delegate
	 * to the underlying iterate */
	struct shall_file_data * fd = dir->private_data;
	if (! fd) return -EINVAL;
	return iterate_dir(fd->file, ctx);
}

/* Determine the path from the root of the filesystem to the file, used
 * by open, create, etc to put a filename in the event log */
static inline char * find_path(struct dentry *dentry, char ** freeit) {
	struct shall_fsinfo * fi = dentry->d_sb->s_fs_info;
	if (SHOW_NAME(fi)) {
		char * buf = shall_getname(dentry->d_sb->s_fs_info), * res;
		if (! buf) return NULL;
		res = dentry_path_raw(dentry, buf, PATH_MAX);
		if (IS_ERR(res)) {
			shall_putname(dentry->d_sb->s_fs_info, buf);
			return NULL;
		}
		*freeit = buf;
		return res;
	} else {
		*freeit = NULL;
		return "";
	}
}

static int shall_open(struct inode *inode, struct file *file) {
	/* open the underlying file and remember their file structure */
	struct path upath;
	struct shall_fsinfo * fi = inode->i_sb->s_fs_info;
	struct shall_file_data * fd;
	if (! inode->i_private) return -EINVAL;
	fd = shall_kmalloc(fi, sizeof(*fd), GFP_KERNEL);
	if (! fd) return -ENOMEM;
	upath.dentry = inode->i_private;
	upath.mnt = fi->root_path.mnt;
	fd->file = dentry_open(&upath, file->f_flags, current_cred());
	if (IS_ERR(fd->file)) {
		int err = PTR_ERR(fd->file);
		shall_kfree(fi, fd);
		return err;
	}
	/* we don't log open operation, even when opening for write;
	 * the first write will however log both the open and itself;
	 * fd->had_id will tell us whether it's the first write (0)
	 * or not (1); the write log itself could be cached under
	 * some conditions, and fd->cached_log tells us whether there
	 * is a log to flush */
	fd->has_id = 0;
	/* determine log mode from mount options */
	if (fi->options.flags & DATA_HASH)
	    fd->log_mode = shall_log_hash;
	else if (fi->options.flags & DATA_FULL)
	    fd->log_mode = shall_log_data;
	else
	    fd->log_mode = shall_log_op;
	fd->fi = fi;
	fd->cached_log = 0;
	file->private_data = fd;
	return 0;
}

/* common code for create, mkdir, mknod */
static int make_node(struct inode *dir, struct dentry *dentry, umode_t mode,
		     bool exclusive, int operation, dev_t dev)
{
	struct shall_attr attr;
	struct shall_fsinfo * fi = dir->i_sb->s_fs_info;
	struct dentry * u_dir = dir->i_private, * u_dentry;
	char * freeit, * path = find_path(dentry, &freeit);
	int res, early_unlock = operation != SHALL_CREATE;
	if (! path) return -ENOMEM;
	if (! u_dir) {
		res = -EINVAL;
		early_unlock = 1;
		goto failed;
	}
	lock_inode_nested(u_dir->d_inode, I_MUTEX_PARENT);
	/* do a lookup so we have a dentry in the underlying filesystem */
	u_dentry = lookup_one_len(dentry->d_name.name, u_dir,
				  dentry->d_name.len);
	if (early_unlock) unlock_inode(u_dir->d_inode);
	if (IS_ERR(u_dentry)) {
		res = PTR_ERR(u_dentry);
		goto failed;
	}
	memset(&attr, 0, sizeof(attr));
	attr.flags = shall_attr_mode;
	if (exclusive) attr.flags |= shall_attr_excl;
	attr.mode = mode;
	if (operation == SHALL_MKNOD) {
		attr.flags |=
			S_ISCHR(mode) ? shall_attr_char : shall_attr_block;
		attr.device = dev;
	}
	if (IS_LOG_BEFORE(fi)) {
		res = shall_log_1a(fi, -operation, path, &attr, 0);
		if (res < 0) goto failed_dput;
	}
	/* use vfs_create to do the actual create in the underlying
	 * filesystem */
	switch (operation) {
		case SHALL_CREATE :
			res = vfs_create(u_dir->d_inode, u_dentry,
					 mode, exclusive);
			break;
		case SHALL_MKDIR :
			res = vfs_mkdir(u_dir->d_inode, u_dentry, mode);
			break;
		case SHALL_MKNOD :
			res = vfs_mknod(u_dir->d_inode, u_dentry, mode, dev);
			break;
		default :
			res = -ENOSYS;
			break;
	}
	if (! res) {
		/* use the result of vfs_create to fill up our inode */
		struct inode * inode = shall_new_inode(fi->sb, u_dentry);
		if (IS_ERR(inode)) {
			res = PTR_ERR(inode);
		} else {
			attr.user = i_uid_read(inode);
			attr.group = i_gid_read(inode);
			attr.atime_sec = inode->i_atime.tv_sec;
			attr.atime_nsec = inode->i_atime.tv_nsec;
			attr.mtime_sec = inode->i_mtime.tv_sec;
			attr.mtime_nsec = inode->i_mtime.tv_nsec;
			attr.flags |= shall_attr_user
				   |  shall_attr_group
				   |  shall_attr_atime
				   |  shall_attr_mtime;
			d_instantiate(dentry, inode);
		}
	}
	if (IS_LOG_AFTER(fi))
		shall_log_1a(fi, operation, path, &attr, res);
failed_dput:
	dput(u_dentry);
failed:
	if (freeit) shall_putname(fi, freeit);
	if (! early_unlock) unlock_inode(u_dir->d_inode);
	return res;
}

static int shall_create(struct inode *dir, struct dentry *dentry,
			umode_t mode, bool excl)
{
	return make_node(dir, dentry, mode, excl, SHALL_CREATE, MKDEV(0, 0));
}

static int shall_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode) {
	return make_node(dir, dentry, mode, false, SHALL_MKDIR, MKDEV(0, 0));
}

static int shall_mknod(struct inode *dir, struct dentry *dentry,
		       umode_t mode, dev_t dev)
{
	return make_node(dir, dentry, mode, false, SHALL_MKNOD, dev);
}

static loff_t shall_llseek(struct file *file, loff_t pos, int whence) {
	/* the underlying filesystem may have a non-standard seek */
	struct shall_file_data * fd = file->private_data;
	if (! fd) return -EINVAL;
	if (fd->file->f_op && fd->file->f_op->llseek)
		return fd->file->f_op->llseek(fd->file, pos, whence);
	/* no special seek, use generic */
	return generic_file_llseek(fd->file, pos, whence);
}

static ssize_t shall_read(struct file *file, char __user *dest,
			  size_t len, loff_t *pos)
{
	struct shall_file_data * fd = file->private_data;
	if (! fd) return 0;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 14, 0)
	/* use vfs_read rather than the underlying fop->read,
	 * because the filesystem may implement aio_read or read_iter
	 * instead of read XXX need to figure out which version stopped
	 * exporting vfs_read() */
	return vfs_read(fd->file, dest, len, pos);
#else
	/* vfs_read is no longer exported to modules, and kernel_read
	 * expects a kernel pointer, so we have to re-do work already
	 * done by vfs_read; at least we don't need to verify the buffer
	 * as our caller has done that already */
	if (fd->file->f_op->read)
		return fd->file->f_op->read(fd->file, dest, len, pos);
	if (fd->file->f_op->read_iter) {
		struct iovec iov = {
			.iov_base = (void __user *)dest,
			.iov_len = len
		};
		struct kiocb kiocb;
		struct iov_iter iter;
		ssize_t ret;
		init_sync_kiocb(&kiocb, fd->file);
		kiocb.ki_pos = *pos;
		iov_iter_init(&iter, READ, &iov, 1, len);
		ret = call_read_iter(fd->file, &kiocb, &iter);
		*pos = kiocb.ki_pos;
		return ret;
	}
	return -ENOSYS;
#endif
}

/* emit a cached WRITE log */
static inline int log_previous(struct shall_fsinfo *fi,
			       struct shall_file_data *fd)
{
	int err;
	if (! fd->cached_log) return 0;
	err = shall_log_0r(fi, SHALL_WRITE, fd->start, fd->length, fd->id, 0);
	fd->cached_log = 0;
	return err;
}

/* see if a cached WRITE log can be extended to cover a new operation */
static inline int extend_log(struct shall_file_data *fd,
			     loff_t start, size_t len)
{
	loff_t next;
	if (! fd->cached_log) return 0;
	if (start > fd->next) return 0;
	next = start + len;
	if (next < fd->start) return 0;
	if (start < fd->start) fd->start = start;
	if (next > fd->next) fd->next = next;
	fd->length = fd->next - fd->start;
	return 1;
}

static inline int log_writes(struct shall_file_data *fd) {
	if (! fd || fd->log_mode == shall_log_none) return 0;
	if (fd->file && fd->file->f_inode && fd->file->f_inode->i_nlink > 0)
		return 1;
	fd->log_mode = shall_log_none;
	return 0;
}

static ssize_t log_write_data(struct shall_fsinfo *fi,
			      struct shall_file_data *fd,
			      shall_log_mode_t log_mode,
			      int operation, loff_t start, size_t length,
			      int fileid, int result, const char __user *src)
{
	ssize_t res = log_previous(fi, fd);
	if (res) return res;
	switch (log_mode) {
		case shall_log_none :
			return 0;
		case shall_log_op :
			return shall_log_0r(fi, operation, start,
					    length, fd->id, result);
		case shall_log_hash :
			return shall_log_0h(fi, operation, start,
					    length, src, fd->id, result);
		case shall_log_data :
			return shall_log_0d(fi, operation, start,
					    length, src, fd->id, result);
	}
	return -EINVAL; /* shouldn't happen (TM) */
}

static ssize_t shall_write(struct file *file, const char __user *src,
			   size_t len, loff_t *pos)
{
	struct shall_file_data * fd = file->private_data;
	struct shall_fsinfo * fi = fd->fi;
	loff_t oldpos = *pos;
	ssize_t res;
	shall_log_mode_t log_mode;
	if (! fd) return -EPIPE;
	if (! fd->has_id && log_writes(fd)) {
		char * freeit, * path = find_path(file->f_path.dentry, &freeit);
		if (! path) return -ENOMEM;
		fd->has_id = 1;
		fd->cached_log = 0;
		fd->id = atomic_inc_return(&last_fileid);
		shall_log_1i(fi, SHALL_OPEN, path, fd->id, 0);
		if (freeit) shall_putname(fi, freeit);
	}
	log_mode = fd->log_mode;
	if (IS_LOG_BEFORE(fi) && log_writes(fd)) {
		res = log_write_data(fi, fd, log_mode, -SHALL_WRITE,
				     oldpos, len, fd->id, 0, src);
		if (res) return res;
		log_mode = shall_log_none; /* no need to log data twice */
	}
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 14, 0)
	/* use vfs_write rather than the underlying fop->write,
	 * because the filesystem may implement aio_write or write_iter
	 * instead of write XXX need to check which exact kernel version
	 * stopped exporting vfs_write() */
	res = vfs_write(fd->file, src, len, pos);
#else
	/* vfs_write is no longer exported to modules, and kernel_write
	 * expects a kernel pointer, so we have to re-do work already
	 * done by vfs_write; at least we don't need to verify the buffer
	 * as our caller has done that already */
	if (fd->file->f_op->write) {
		res = fd->file->f_op->write(fd->file, src, len, pos);
	} else if (fd->file->f_op->write_iter) {
		struct iovec iov = {
			.iov_base = (void __user *)src,
			.iov_len = len
		};
		struct kiocb kiocb;
		struct iov_iter iter;
		ssize_t ret;
		init_sync_kiocb(&kiocb, fd->file);
		kiocb.ki_pos = *pos;
		iov_iter_init(&iter, WRITE, &iov, 1, len);
		ret = call_write_iter(fd->file, &kiocb, &iter);
		if (ret > 0)
			*pos = kiocb.ki_pos;
	} else {
		/* sorry... */
		res = -ENOSYS;
	}
#endif
	if (IS_LOG_AFTER(fi) && log_writes(fd)) {
		if (res < 0 || log_mode != shall_log_op) {
			res = log_write_data(fi, fd, log_mode, SHALL_WRITE,
					     oldpos, len, fd->id, (int)res, src);
		} else {
			/* if we can extend the cached log, nothing to do;
			 * if we cannot extend it we must flush it and make
			 * a new one */
			if (! extend_log(fd, oldpos, res)) {
				log_previous(fi, fd);
				fd->cached_log = 1;
				fd->start = oldpos;
				fd->length = res;
				fd->next = oldpos + res;
			}
		}
	}
	return res;
}

static unsigned int shall_poll(struct file *file, poll_table *poll) {
	/* call poll() on the underlying file, if possible, otherwise
	 * return default mask */
	struct shall_file_data * fd = file->private_data;
	if (! fd) return 0;
	if (fd->file->f_op && fd->file->f_op->poll)
		return fd->file->f_op->poll(fd->file, poll);
	return DEFAULT_POLLMASK;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 0, 0)
static int shall_show_fdinfo(struct seq_file *m, struct file *file) {
#else
static void shall_show_fdinfo(struct seq_file *m, struct file *file) {
#endif
	/* if the underlying file has a fdinfo function, call it;
	 * otherwise NOP */
	char * freeit = NULL;
	const char * path = "";
	struct dentry * dentry;
	struct shall_file_data * fd = file->private_data;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 0, 0)
	int ret = 0;
#endif
	if (fd && fd->file->f_op && fd->file->f_op->show_fdinfo)
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 0, 0)
		ret =
#endif
		fd->file->f_op->show_fdinfo(m, fd->file);
	dentry = d_find_alias(file->f_inode);
	if (dentry) {
		path = find_path(dentry, &freeit);
		if (! path) path = "";
		dput(dentry);
	}
	seq_printf(m, "shallfs: %s%s\n", fd->fi->options.fspath, path);
	if (freeit) shall_putname(fd->fi, freeit);
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 0, 0)
	return ret;
#endif
}

/* any close on file, but file may remain open with another descriptor */
static int shall_flush(struct file *file, fl_owner_t id) {
	/* flush any pending write log and then see if the underlying
	 * file has a flush operation */
	int ret = 0;
	if (file->private_data) {
		struct shall_file_data * fd = file->private_data;
		if (fd->has_id && log_writes(fd)) {
			log_previous(fd->fi, fd);
			shall_log_0i(fd->fi, SHALL_COMMIT, fd->id, 0);
		}
		if (fd->file->f_op && fd->file->f_op->flush)
			ret = fd->file->f_op->flush(fd->file, id);
	}
	return ret;
}

/* last close on a file */
static int shall_release(struct inode *inode, struct file *file) {
	/* we need to release the underlying file and free our stuff */
	if (file->private_data) {
		struct shall_file_data * fd = file->private_data;
		/* just check for has_id, not log_writes(fd), because if
		 * we started logging we must finish it, even if we
		 * stopped logging mid-way; however, no point flushing
		 * previous log if we stopped logging */
		if (fd->has_id) {
			if (fd->cached_log && log_writes(fd))
				log_previous(fd->fi, fd);
			shall_log_0i(fd->fi, SHALL_CLOSE, fd->id, 0);
		}
		fput(fd->file);
		shall_kfree(fd->fi, fd);
	}
	return 0;
}

/* attribute change and/or truncate/extend */
static int shall_setattr(struct dentry *dentry, struct iattr *iattr) {
	struct shall_attr attr;
	struct dentry * u_dentry;
	struct shall_fsinfo * fi = dentry->d_sb->s_fs_info;
	char * freeit, * path = find_path(dentry, &freeit);
	int err;
	if (! path) return -ENOMEM;
	/* just call setattr on the underlying file */
	if (unlikely(! dentry->d_inode || ! dentry->d_inode->i_private)) {
		if (freeit) shall_putname(fi, freeit);
		return -EINVAL;
	}
	attr.flags = 0;
	if (iattr->ia_valid & ATTR_MODE) {
		attr.flags |= shall_attr_mode;
		attr.mode = iattr->ia_mode;
	}
	if (iattr->ia_valid & ATTR_UID) {
		attr.flags |= shall_attr_user;
		attr.user = from_kuid(&init_user_ns, iattr->ia_uid);
	}
	if (iattr->ia_valid & ATTR_GID) {
		attr.flags |= shall_attr_group;
		attr.group = from_kgid(&init_user_ns, iattr->ia_gid);
	}
	if (iattr->ia_valid & ATTR_SIZE) {
		attr.flags |= shall_attr_size;
		attr.size = iattr->ia_size;
	}
	if (iattr->ia_valid & ATTR_ATIME) {
		attr.flags |= shall_attr_atime;
		attr.atime_sec = iattr->ia_atime.tv_sec;
		attr.atime_nsec = iattr->ia_atime.tv_nsec;
	}
	if (iattr->ia_valid & ATTR_MTIME) {
		attr.flags |= shall_attr_mtime;
		attr.mtime_sec = iattr->ia_mtime.tv_sec;
		attr.mtime_nsec = iattr->ia_mtime.tv_nsec;
	}
	if (IS_LOG_BEFORE(fi) && attr.flags) {
		err = shall_log_1a(fi, -SHALL_META, path, &attr, 0);
		if (err) goto out;
	}
	u_dentry = dentry->d_inode->i_private;
	lock_inode(u_dentry->d_inode);
	err = notify_change(u_dentry, iattr, NULL);
	unlock_inode(u_dentry->d_inode);
	if (IS_LOG_AFTER(fi) && attr.flags)
		shall_log_1a(fi, SHALL_META, path, &attr, err);
out:
	if (freeit) shall_putname(fi, freeit);
	return err;
}

/* update file times, a subset of setattr but handled specially */
static int shall_update_time(struct inode *inode, struct_timespec *tm, int fl) {
	struct shall_attr attr;
	struct shall_fsinfo * fi = inode->i_sb->s_fs_info;
	struct dentry * u_dentry = inode->i_private;
	struct inode * u_inode;
	struct dentry * dentry = d_find_alias(inode);
	char * freeit, * path;
	int err = 0;
	if (! dentry) return -ENOENT;
	if (! u_dentry) {
		dput(dentry);
		return -EINVAL;
	}
	u_inode = u_dentry->d_inode;
	path = find_path(dentry, &freeit);
	dput(dentry);
	if (! path) return -ENOMEM;
	attr.flags = 0;
	if (fl & S_ATIME) {
		attr.flags |= shall_attr_atime;
		attr.atime_sec = tm->tv_sec;
		attr.atime_nsec = tm->tv_nsec;
		inode->i_atime = *tm;
	}
	if (fl & S_VERSION)
		inode_inc_iversion(inode);
	if (fl & S_MTIME) {
		attr.flags |= shall_attr_mtime;
		attr.mtime_sec = tm->tv_sec;
		attr.mtime_nsec = tm->tv_nsec;
		inode->i_mtime = *tm;
	}
	if (fl & S_CTIME)
		inode->i_ctime = *tm;
	if (IS_LOG_BEFORE(fi) && attr.flags) {
		err = shall_log_1a(fi, -SHALL_META, path, &attr, 0);
		if (err) goto out;
	}
	mark_inode_dirty_sync(inode);
	if (u_inode && u_inode->i_op && u_inode->i_op->update_time) {
		err = u_inode->i_op->update_time(u_inode, tm, fl);
	} else {
		if (fl & S_ATIME)
			u_inode->i_atime = *tm;
		if (fl & S_VERSION)
			inode_inc_iversion(u_inode);
		if (fl & S_MTIME)
			u_inode->i_mtime = *tm;
		if (fl & S_CTIME)
			u_inode->i_ctime = *tm;
		mark_inode_dirty_sync(u_inode);
	}
	if (IS_LOG_AFTER(fi) && attr.flags)
		shall_log_1a(fi, SHALL_META, path, &attr, err);
out:
	if (freeit) shall_putname(fi, freeit);
	return err;
}

/* get attributes */
/* we'll need to see exactly which kernel version for this... XXX */
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 14, 0)
static int shall_getattr(struct vfsmount *mnt, struct dentry *dentry,
			 struct kstat *kstat)
{
	struct path upath;
	struct shall_fsinfo * fi = mnt->mnt_sb->s_fs_info;
	int err;
	if (unlikely(! dentry->d_inode || ! dentry->d_inode->i_private))
		return -EINVAL;
	upath.dentry = dentry->d_inode->i_private;
	upath.mnt = fi->root_path.mnt;
	err = vfs_getattr(&upath, kstat);
	if (! err) kstat->dev = mnt->mnt_sb->s_dev;
	return err;
}
#else
static int shall_getattr(const struct path * path, struct kstat *kstat,
			 u32 request_mask, unsigned int query_flags)
{
	struct path upath;
	struct dentry * dentry = path->dentry;
	struct shall_fsinfo * fi = path->mnt->mnt_sb->s_fs_info;
	int err;
	if (unlikely(! dentry || ! dentry->d_inode || ! dentry->d_inode->i_private))
		return -EINVAL;
	upath.dentry = dentry->d_inode->i_private;
	upath.mnt = fi->root_path.mnt;
	err = vfs_getattr(&upath, kstat, request_mask, query_flags);
	if (! err) kstat->dev = path->mnt->mnt_sb->s_dev;
	return err;
}
#endif

static struct posix_acl * shall_get_acl(struct inode *inode, int type) {
	struct dentry * u_dentry = inode->i_private;
	struct inode * u_inode;
	if (! u_dentry) return ERR_PTR(-EINVAL);
	u_inode = u_dentry->d_inode;
	if (u_inode && u_inode->i_op && u_inode->i_op->get_acl)
		return u_inode->i_op->get_acl(u_inode, type);
	return ERR_PTR(-ENOSYS);
}

static int shall_set_acl(struct inode *inode, struct posix_acl *acl, int type) {
	struct dentry * u_dentry = inode->i_private;
	struct inode * u_inode;
	if (! u_dentry) return -EINVAL;
	u_inode = u_dentry->d_inode;
	if (u_inode && u_inode->i_op && u_inode->i_op->set_acl) {
		struct shall_fsinfo * fi = inode->i_sb->s_fs_info;
		struct dentry * dentry = d_find_alias(inode);
		char * freeit, * path;
		int err;
		enum shall_acl_flags flags;
		if (! dentry) return -ENOENT;
		path = find_path(dentry, &freeit);
		dput(dentry);
		if (! path) return -ENOMEM;
		switch (type) {
			case ACL_TYPE_ACCESS :
				flags = shall_acl_access;
				break;
			case ACL_TYPE_DEFAULT :
				flags = shall_acl_default;
				break;
			default :
				return -EINVAL;
		}
		if (IS_LOG_BEFORE(fi)) {
			err = shall_log_1l(fi, -SHALL_SET_ACL,
					   path, flags, acl, 0);
			if (err) goto out;
		}
		err = u_inode->i_op->set_acl(u_inode, acl, type);
		if (IS_LOG_AFTER(fi))
			shall_log_1l(fi, SHALL_SET_ACL, path, flags, acl, err);
	out:
		if (freeit) shall_putname(fi, freeit);
		return err;
	}
	return -ENOSYS;
}

static int shall_rename(struct inode *olddir, struct dentry *oldname,
			struct inode *newdir, struct dentry *newname,
			unsigned int flags)
{
	struct dentry * u_olddirdentry = olddir->i_private, * u_oldname;
	struct dentry * u_newdirdentry = newdir->i_private, * u_newname;
	struct dentry * maybe_parent;
	struct inode * dbreak = NULL;
	struct shall_fsinfo * fi = olddir->i_sb->s_fs_info;
	char * oldfree, * oldpath, * newfree, * newpath;
	int err;
	int operation = (flags & RENAME_EXCHANGE) ? SHALL_SWAP : SHALL_MOVE;
	if (! u_olddirdentry || ! u_newdirdentry) return -EINVAL;
	err = -ENOMEM;
	oldpath = find_path(oldname, &oldfree);
	if (! oldpath) goto fail;
	newpath = find_path(newname, &newfree);
	if (! newpath) goto fail_putname_old;
	if (IS_LOG_BEFORE(fi)) {
		err = shall_log_2n(fi, -operation, oldpath, newpath, 0);
		if (err) goto fail_putname_new;
	}
retry_break:
	/* see the comments in vfs_rename() about locking;  this is where
	 * one can no longer see the difference beteen BSD and LSD */
	maybe_parent = lock_rename(u_olddirdentry, u_newdirdentry);
	u_oldname = lookup_one_len(oldname->d_name.name, u_olddirdentry,
				   oldname->d_name.len);
	if (IS_ERR(u_oldname)) {
		err = PTR_ERR(u_oldname);
		goto out_unlock;
	}
	if (d_is_negative(u_oldname)) {
		/* ... because of course a lookup of something which does
		 * not exist returns success */
		err = -ENOENT;
		goto out_dput_old;
	}
	if (maybe_parent == u_oldname) {
		/* caller has already checked... except it was for "our"
		 * paths, not the underlying filesystem's: which supposedly
		 * is the same, unless somebody went and changed things
		 * while we weren't looking */
		err = -EINVAL;
		goto out_dput_old;
	}
	u_newname = lookup_one_len(newname->d_name.name, u_newdirdentry,
				   newname->d_name.len);
	if (IS_ERR(u_newname)) {
		err = PTR_ERR(u_newname);
		goto out_dput_old;
	}
	if ((flags & RENAME_NOREPLACE) && d_is_positive(u_newname)) {
		err = -EEXIST;
		goto out_dput_new;
	}
	if ((flags & RENAME_EXCHANGE) && d_is_negative(u_newname)) {
		err = -ENOENT;
		goto out_dput_new;
	}
	if (maybe_parent == u_newname) {
		if (flags & RENAME_EXCHANGE)
			err = -EINVAL;
		else
			err = -ENOTEMPTY;
		goto out_dput_new;
	}
	err = vfs_rename(u_olddirdentry->d_inode, u_oldname,
			 u_newdirdentry->d_inode, u_newname,
			 &dbreak, flags);
out_dput_new:
	dput(u_newname);
out_dput_old:
	dput(u_oldname);
out_unlock:
	unlock_rename(u_olddirdentry, u_newdirdentry);
	if (dbreak) {
		err = break_deleg_wait(&dbreak);
		if (! err) goto retry_break;
	}
	if (IS_LOG_AFTER(fi))
		shall_log_2n(fi, operation, oldpath, newpath, err);
fail_putname_new:
	if (newfree) shall_putname(fi, newfree);
fail_putname_old:
	if (oldfree) shall_putname(fi, oldfree);
fail:
	return err;
}

static int shall_symlink(struct inode *dir, struct dentry *dentry,
			 const char *target)
{
	struct shall_attr attr;
	struct dentry * u_dirdentry = dir->i_private, * u_dentry;
	struct inode * u_dir;
	struct shall_fsinfo * fi = dir->i_sb->s_fs_info;
	char * freeit, * path = find_path(dentry, &freeit);
	int err;
	if (! path) return -ENOMEM;
	if (! u_dirdentry || dentry->d_inode) {
		err = -EINVAL;
		goto out_putname;
	}
	u_dir = u_dirdentry->d_inode;
	if (! u_dir) {
		err = -ENOENT;
		goto out_putname;
	}
	lock_inode(u_dir);
	u_dentry = lookup_one_len(dentry->d_name.name, u_dirdentry,
				  dentry->d_name.len);
	unlock_inode(u_dir);
	if (IS_ERR(u_dentry)) {
		err = PTR_ERR(u_dentry);
		goto out_putname;
	}
	attr.flags = 0;
	if (IS_LOG_BEFORE(fi)) {
		err = shall_log_2a(fi, -SHALL_SYMLINK, path, target, &attr, 0);
		if (err) goto out_dput;
	}
	err = vfs_symlink(u_dir, u_dentry, target);
	if (err == 0) {
		struct inode * inode = shall_new_inode(dir->i_sb, u_dentry);
		if (IS_ERR(inode)) {
			err = PTR_ERR(inode);
			goto out_dput;
		}
		attr.mode = inode->i_mode;
		attr.user = i_uid_read(inode);
		attr.group = i_gid_read(inode);
		attr.atime_sec = inode->i_atime.tv_sec;
		attr.atime_nsec = inode->i_atime.tv_nsec;
		attr.mtime_sec = inode->i_mtime.tv_sec;
		attr.mtime_nsec = inode->i_mtime.tv_nsec;
		attr.flags |= shall_attr_mode
			   |  shall_attr_user
			   |  shall_attr_group
			   |  shall_attr_atime
			   |  shall_attr_mtime;
		d_instantiate(dentry, inode);
	}
	if (IS_LOG_AFTER(fi))
		shall_log_2a(fi, SHALL_SYMLINK, path, target, &attr, err);
out_dput:
	dput(u_dentry);
out_putname:
	if (freeit) shall_putname(fi, freeit);
	return err;
}

static int shall_link(struct dentry *olddentry,
		      struct inode *newdir, struct dentry *newdentry)
{
	struct shall_fsinfo * fi = olddentry->d_sb->s_fs_info;
	struct dentry * u_newdirdentry = newdir->i_private, * u_newdentry;
	struct dentry * u_olddentry = olddentry->d_inode->i_private;
	struct inode * dbreak = NULL, * newinode;
	char * oldfree, * oldpath, * newfree, * newpath;
	int err;
	if (! u_olddentry || ! u_newdirdentry) return -EINVAL;
	err = -ENOMEM;
	oldpath = find_path(olddentry, &oldfree);
	if (! oldpath) goto fail;
	newpath = find_path(newdentry, &newfree);
	if (! newpath) goto fail_putname_old;
	if (IS_LOG_BEFORE(fi)) {
		err = shall_log_2n(fi, -SHALL_LINK, oldpath, newpath, 0);
		if (err) goto fail_putname_new;
	}
retry_break:
	lock_inode(u_newdirdentry->d_inode);
	u_newdentry = lookup_one_len(newdentry->d_name.name, u_newdirdentry,
				     newdentry->d_name.len);
	if (IS_ERR(u_newdentry)) {
		err = PTR_ERR(u_newdentry);
		goto out_unlock;
	}
	err = vfs_link(u_olddentry, u_newdirdentry->d_inode,
		       u_newdentry, &dbreak);
	newinode = shall_new_inode(newdir->i_sb, u_newdentry);
	if (IS_ERR(newinode)) {
		err = PTR_ERR(newinode);
		goto out_dput;
	}
	d_instantiate(newdentry, newinode);
out_dput:
	dput(u_newdentry);
out_unlock:
	unlock_inode(u_newdirdentry->d_inode);
	if (dbreak) {
		err = break_deleg_wait(&dbreak);
		if (! err) goto retry_break;
	}
	if (IS_LOG_AFTER(fi))
		shall_log_2n(fi, SHALL_LINK, oldpath, newpath, err);
fail_putname_new:
	if (newfree) shall_putname(fi, newfree);
fail_putname_old:
	if (oldfree) shall_putname(fi, oldfree);
fail:
	return err;
}

static int remove_node(struct inode *dir, struct dentry *dentry,
		       int operation)
{
	struct dentry * u_dirdentry = dir->i_private, * u_dentry;
	struct inode * u_dir, * dbreak = NULL;
	struct shall_fsinfo * fi = dir->i_sb->s_fs_info;
	char * freeit, * path = find_path(dentry, &freeit);
	int err;
	if (! path) return -ENOMEM;
	if (! u_dirdentry) {
		err = -EINVAL;
		goto out_putname;
	}
	u_dir = u_dirdentry->d_inode;
	if (! u_dir) {
		err = -EINVAL;
		goto out_putname;
	}
	if (IS_LOG_BEFORE(fi)) {
		err = shall_log_1n(fi, -operation, path, 0);
		if (err) goto out_putname;
	}
retry_break:
	if (operation == SHALL_DELETE)
		lock_inode(u_dir);
	else
		lock_inode_nested(u_dir, I_MUTEX_PARENT);
	u_dentry = lookup_one_len(dentry->d_name.name, u_dirdentry,
				  dentry->d_name.len);
	if (IS_ERR(u_dentry)) {
		err = PTR_ERR(u_dentry);
	} else {
		if (operation == SHALL_DELETE)
			err = vfs_unlink(u_dir, u_dentry, &dbreak);
		else
			err = vfs_rmdir(u_dir, u_dentry);
		dput(u_dentry);
	}
	unlock_inode(u_dir);
	if (dbreak) {
		err = break_deleg_wait(&dbreak);
		if (! err) goto retry_break;
	}
	if (IS_LOG_AFTER(fi))
		shall_log_1n(fi, operation, path, err);
out_putname:
	if (freeit) shall_putname(fi, freeit);
	return err;
}

static int shall_unlink(struct inode *dir, struct dentry *dentry) {
	return remove_node(dir, dentry, SHALL_DELETE);
}

static int shall_rmdir(struct inode *dir, struct dentry *dentry) {
	return remove_node(dir, dentry, SHALL_RMDIR);
}

static int shall_fsync(struct file *file, loff_t from, loff_t to, int data) {
	/* we are not logging this, so all we do is pass it on */
	struct shall_file_data * fd = file->private_data;
	if (! fd) return -EINVAL;
	return vfs_fsync_range(fd->file, from, to, data);
}

/* file operations for regular files; pretty much everything except
 * readdir */
static struct file_operations shall_file_file_operations = {
	.owner		= THIS_MODULE,
	.open		= shall_open,
	.llseek		= shall_llseek,
	.read		= shall_read,
	.write		= shall_write,
	.flush		= shall_flush,
	.release	= shall_release,
	.poll		= shall_poll,
	.show_fdinfo	= shall_show_fdinfo,
	.fsync		= shall_fsync,
	// XXX long (*unlocked_ioctl) (struct file *, unsigned int, unsigned long);
	// XXX long (*compat_ioctl) (struct file *, unsigned int, unsigned long);
	// XXX int (*mmap) (struct file *, struct vm_area_struct *);
	// XXX void (*mremap)(struct file *, struct vm_area_struct *);
	// XXX int (*fasync) (int, struct file *, int);
	// XXX int (*lock) (struct file *, int, struct file_lock *);
	// XXX unsigned long (*get_unmapped_area)(struct file *, unsigned long, unsigned long, unsigned long, unsigned long);
	// XXX int (*check_flags)(int);
	// XXX int (*flock) (struct file *, int, struct file_lock *);
	// XXX int (*setlease)(struct file *, long, struct file_lock **, void **);
	// XXX long (*fallocate)(struct file *file, int mode, loff_t offset, loff_t len);
};

/* file operations for directories; only a small amount of these make
 * sense */
static struct file_operations shall_dir_file_operations = {
	.owner		= THIS_MODULE,
	.iterate	= shall_iterate,
	.open		= shall_open,
	.llseek		= shall_llseek,
	.read		= generic_read_dir,
	.release	= shall_release,
	.fsync		= shall_fsync
	// XXX long (*unlocked_ioctl) (struct file *, unsigned int, unsigned long);
	// XXX long (*compat_ioctl) (struct file *, unsigned int, unsigned long);
};

/* inode operation for "other" types of files, which in this context means
 * anything except directories and symlinks */
static struct inode_operations shall_other_inode_operations = {
	.update_time	= shall_update_time,
	.get_acl	= shall_get_acl,
	.set_acl	= shall_set_acl,
	.setattr	= shall_setattr,
	.getattr	= shall_getattr,
	.listxattr	= generic_listxattr,
#if SHALL_USE_OLD_XATTR_CODE
	.getxattr	= generic_getxattr,
	.setxattr	= generic_setxattr,
	.removexattr	= generic_removexattr,
#endif
};

/* inode operations for directories; they can do pretty much everything
 * except readlink and follow_link (we must make sure these are not defined
 * or the kernel will try to treat our directories as symlinks!) */
static struct inode_operations shall_dir_inode_operations = {
	.lookup		= shall_lookup,
	.create		= shall_create,
	.update_time	= shall_update_time,
	.get_acl	= shall_get_acl,
	.set_acl	= shall_set_acl,
	.setattr	= shall_setattr,
	.getattr	= shall_getattr,
	.listxattr	= generic_listxattr,
#if SHALL_USE_OLD_XATTR_CODE
	.getxattr	= generic_getxattr,
	.setxattr	= generic_setxattr,
	.removexattr	= generic_removexattr,
#endif
#if SHALL_USE_OLD_RENAME_CODE
	.rename		= NULL, /* because we use rename2 */
	.rename2	= shall_rename,
#else
	.rename		= shall_rename,
#endif
	.link		= shall_link,
	.unlink		= shall_unlink,
	.symlink	= shall_symlink,
	.mkdir		= shall_mkdir,
	.rmdir		= shall_rmdir,
	.mknod		= shall_mknod,
	// XXX int (*atomic_open)(struct inode *, struct dentry *, struct file *, unsigned open_flag, umode_t create_mode, int *opened);
	// XXX int (*tmpfile) (struct inode *, struct dentry *, umode_t);
};

/* inode operations for symlinks; since symlinks have no inherent
 * permissions, acl calls are not specified; however they must have
 * readlink and follow_link if they are going to be of any use */
static struct inode_operations shall_symlink_inode_operations = {
	.lookup		= shall_lookup,
	.readlink	= shall_readlink,
#if SHALL_USE_OLD_SYMLINK_CODE
	.follow_link	= shall_follow_link,
	.put_link	= shall_put_link,
#else
	.get_link	= shall_get_link,
#endif
	.setattr	= shall_setattr,
	.getattr	= shall_getattr,
	.listxattr	= generic_listxattr,
#if SHALL_USE_OLD_XATTR_CODE
	.getxattr	= generic_getxattr,
	.setxattr	= generic_setxattr,
	.removexattr	= generic_removexattr,
#endif
};

void shall_evict_inode(struct inode *inode) {
	if (inode->i_private) {
		dput(inode->i_private);
		inode->i_private = NULL;
	}
	/* this isn't done by the kernel if we have evict_inode... */
	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);
}

#if SHALL_USE_OLD_XATTR_CODE
 /* newer kernels have a struxt xattr_handler as first arg to functions */
# if LINUX_VERSION_CODE < KERNEL_VERSION(4, 4, 0)
#  define SHALL_XATTR_HANDLER /* nothing */
#  define SHALL_XATTR_FLAGS , int handler_flags
# else
#  define SHALL_XATTR_HANDLER const struct xattr_handler *handler,
#  define SHALL_XATTR_FLAGS /* nothing */
# endif
# define SHALL_XATTR_INODE /* nothing */
# define SHALL_XATTR_DECLARE_INODE struct inode * inode = dentry->d_inode;

static size_t shall_listxattr(SHALL_XATTR_HANDLER
			      struct dentry *dentry, char *list,
			      size_t list_size, const char *name,
			      size_t name_len SHALL_XATTR_FLAGS)
{
	struct inode * inode = dentry->d_inode;
	struct dentry * u_dentry;
	if (! inode) return -EINVAL;
	u_dentry = inode->i_private;
	if (! u_dentry) return -EINVAL;
	return vfs_listxattr(u_dentry, list, list_size);
}

#else /* SHALL_USE_OLD_XATTR_CODE */
# define SHALL_XATTR_HANDLER const struct xattr_handler *handler,
# define SHALL_XATTR_FLAGS /* nothing */
# define SHALL_XATTR_INODE struct inode *inode, 
# define SHALL_XATTR_DECLARE_INODE /* nothing */

static bool shall_listxattr(struct dentry *dentry) {
	return true;
}

#endif /* SHALL_USE_OLD_XATTR_CODE */

static int shall_getxattr(SHALL_XATTR_HANDLER struct dentry *dentry,
			  SHALL_XATTR_INODE const char *name,
			  void *buffer, size_t size SHALL_XATTR_FLAGS)
{
	SHALL_XATTR_DECLARE_INODE
	struct dentry * u_dentry;
	if (! inode) return -EINVAL;
	u_dentry = inode->i_private;
	if (! u_dentry) return -EINVAL;
	return vfs_getxattr(u_dentry, name, buffer, size);
}

static int shall_setxattr(SHALL_XATTR_HANDLER struct dentry *dentry,
			  SHALL_XATTR_INODE const char *name,
			  const void *buffer, size_t size,
			  int flags SHALL_XATTR_FLAGS)
{
	SHALL_XATTR_DECLARE_INODE
	struct dentry * u_dentry;
	struct shall_fsinfo * fi = dentry->d_sb->s_fs_info;
	char * freeit, * path = find_path(dentry, &freeit);
	int err;
	if (! path) return -ENOMEM;
	err = -EINVAL;
	if (! inode) goto out_putname;
	u_dentry = inode->i_private;
	if (! u_dentry) goto out_putname;
	if (buffer) {
		/* set attribute */
		if (IS_LOG_BEFORE(fi)) {
			err = shall_log_1x(fi, -SHALL_SET_XATTR, path,
					   name, buffer, size, flags, 0);
			if (err) goto out_putname;
		}
		err = vfs_setxattr(u_dentry, name, buffer, size, flags);
		if (IS_LOG_AFTER(fi))
			shall_log_1x(fi, SHALL_SET_XATTR, path,
				     name, buffer, size, flags, err);
	} else {
		/* delete attribute */
		if (IS_LOG_BEFORE(fi))
			shall_log_2n(fi, -SHALL_DEL_XATTR, path, name, 0);
		err = vfs_removexattr(u_dentry, name);
		if (IS_LOG_AFTER(fi))
			shall_log_2n(fi, SHALL_DEL_XATTR, path, name, err);
	}
out_putname:
	if (freeit) shall_putname(fi, freeit);
	return err;
}

const struct xattr_handler shall_xattr_handler = {
	.prefix	= "",
	.flags	= 0,
	.list	= shall_listxattr,
	.get	= shall_getxattr,
	.set	= shall_setxattr,
#if ! SHALL_USE_OLD_XATTR_CODE
	.name	= "shall",
#endif
};

