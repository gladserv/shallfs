/* linux/fs/shallfs/proc.c
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * this file is part of SHALLFS
 *
 * Copyright (c) 2017-2019 Claudio Calvelli <shallfs@gladserv.com>
 *
 *  similar to linux/fs/proc/array.c (GPL)
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

#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/poll.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/atomic.h>
#include <linux/ctype.h>
#include <shallfs/device.h>
#include <shallfs/operation.h>
#include "shallfs.h"
#include "super.h"
#include "log.h"
#include "device.h"
#include "proc.h"

typedef ssize_t (*get_logs_t)(struct shall_fsinfo *, char __user *, size_t);

/* structure used by log readers so they can be notified of umounts */
struct shall_proc_user {
	struct shall_fsinfo * fi;
	get_logs_t get;
};

/* structure used by list_mounts() */
struct shall_mountlist {
	struct shall_mountlist * next;
	char line[0];
};

/* structure used for the /proc/shallfs/<device>/info file; we need to
 * make a copy of the data in case they unmount while we are reading */
struct shall_info {
	struct timespec mounted; /* time we were last mounted */
	int64_t version;	/* current superblock version */
	loff_t maxsize;		/* max size of the journal */
	loff_t size;		/* current size of the journal */
	loff_t space;		/* space available in journal */
	loff_t devsize;		/* size of device */
	loff_t start;		/* current start of journal data */
	int flags;		/* current superblock flags */
	int logged;		/* number of operations logged */
	int nsuper;		/* number of superblocks */
	int align;		/* default log alignment */
	int commit_size;	/* number of commits because size exceeded */
	int commit_time;	/* number of commits because time exceeded */
	int commit_forced;	/* number of commits on remount etc. */
	char fs[0];		/* underlying filesystem path */
};

/* encode a path */
static int encode(const char * path, char * dest) {
	int len = 0;
	while (*path) {
		char c = *path++;
		if (isascii(c) && isgraph(c) && c != '%') {
			if (dest) *dest++ = c;
			len++;
		} else {
			if (dest) {
				snprintf(dest, 4, "%%%02x", (unsigned char)c);
				dest += 3;
			}
			len += 3;
		}
	}
	return len;
}

/* see mounted_start(); */
static struct shall_mountlist * list_mounts(const struct shall_fsinfo *fi) {
	char dev[32];
	struct shall_mountlist * result = NULL, * last = NULL;
	while (fi) {
		int len, dl, pl;
		struct shall_mountlist * next;
		const char * path = fi->options.fspath;
		char * ptr;
		snprintf(dev, sizeof(dev), "%x:%x %d ",
			 MAJOR(fi->sb->s_dev), MINOR(fi->sb->s_dev),
			 (int)strlen(path));
		dl = strlen(dev);
		pl = encode(path, NULL);
		len = dl + pl + 1;
		next = kmalloc(sizeof(*next) + len, GFP_KERNEL);
		if (! next) continue;
		next->next = NULL;
		ptr = next->line;
		strcpy(ptr, dev);
		ptr += dl;
		encode(path, ptr);
		ptr += pl;
		*ptr++ = 0;
		if (last)
			last->next = next;
		else
			result = next;
		last = next;
		fi = fi->next;
	}
	return result;
}

/* there is a race condition between /proc/shallfs/mounted and umount: if
 * we read a filesystem at a time, we may end up trying to read some memory
 * just freed by umount; instead, we generate the whole text in here in
 * a new buffer which can't be freed by umount; because it's just a quick
 * scan through the list, we can just keep the whole list locked for
 * consistency (we cannot lock here and unlock in mounted_stop() as
 * somebody could then open the file and keep it open and cause the
 * whole system to hang); we decide that there is a maximum size for the
 * thing, which we estimate by calling list_mounts with a NULL pointer */
static void * mounted_start(struct seq_file *m, loff_t *_pos) {
	struct shall_fsinfo * fi;
	loff_t pos = *_pos;
	struct shall_mountlist * list = NULL;
	mutex_lock(&shall_fs_mutex);
	fi = shall_fs_list;
	while (pos > 0 && fi) {
		pos--;
		fi = fi->next;
	}
	if (pos == 0 && fi)
		list = list_mounts(fi);
	mutex_unlock(&shall_fs_mutex);
	return list;
}

/* since we had all the list ready, we never have a "next" */
static void * mounted_next(struct seq_file *m, void *data, loff_t *pos) {
	struct shall_mountlist * list;
	if (! data) return NULL;
	list = data;
	data = list->next;
	kfree(list);
	return data;
}

static void mounted_stop(struct seq_file *m, void *data) {
	while (data) {
		struct shall_mountlist * list = data;
		data = list->next;
		kfree(list);
	}
}

static int mounted_show(struct seq_file *m, void *data) {
	struct shall_mountlist * list = data;
	seq_printf(m, "%s\n", list->line);
	return 0;
}

static struct seq_operations mounted_seq_ops = {
	.start		= mounted_start,
	.next		= mounted_next,
	.stop		= mounted_stop,
	.show		= mounted_show,
};

static int mounted_open(struct inode *inode, struct file *file) {
	return seq_open(file, &mounted_seq_ops);
}

struct file_operations shall_proc_mounted = {
	.open		= mounted_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= seq_release,
};

static void * info_start(struct seq_file *m, loff_t *pos) {
	return *pos == 0 ? m : NULL;
}

static void * info_next(struct seq_file *m, void *_p, loff_t *pos) {
	return NULL;
}

static void info_stop(struct seq_file *m, void *_p) {
	/* nothing to do here, but this must be provided */
}

static int info_show(struct seq_file *m, void *_p) {
	struct shall_info * info = m->private;
	seq_printf(m, "mounted: %ld.%09ld\n",
		   (long)info->mounted.tv_sec, (long)info->mounted.tv_nsec);
	seq_printf(m, "logged: %d\n", info->logged);
	seq_printf(m, "maxsize: %lld\n", (long long)info->maxsize);
	seq_printf(m, "size: %lld\n", (long long)info->size);
	seq_printf(m, "space: %lld\n", (long long)info->space);
	seq_printf(m, "devsize: %lld\n", (long long)info->devsize);
	seq_printf(m, "start: %lld\n", (long long)info->start);
	seq_printf(m, "commit_size: %d\n", info->commit_size);
	seq_printf(m, "commit_time: %d\n", info->commit_time);
	seq_printf(m, "commit_forced: %d\n", info->commit_forced);
	seq_printf(m, "version: %lld\n", (long long)info->version);
	seq_printf(m, "flags: %d\n", info->flags);
	seq_printf(m, "nsuper: %d\n", info->nsuper);
	seq_printf(m, "align: %d\n", info->align);
	seq_printf(m, "fs: %s\n", info->fs);
	return 0;
}

static struct seq_operations info_seq_ops = {
	.start		= info_start,
	.next		= info_next,
	.stop		= info_stop,
	.show		= info_show,
};

static int info_open(struct inode *inode, struct file *file) {
	/* first determine which device we need to use */
	struct shall_fsinfo * fi;
	struct shall_info * info;
	int pathlen;
	fi = proc_get_parent_data(inode);
	if (! fi) return -ENOENT;
	mutex_lock(&fi->sbi.mutex);
	pathlen = strlen(fi->options.fspath);
	info = __seq_open_private(file, &info_seq_ops,
				  sizeof(*info) + pathlen + 1);
	if (! info) {
		mutex_unlock(&fi->sbi.mutex);
		return -ENOMEM;
	}
	info->mounted = timespec_sub(current_kernel_time(),
				     fi->sbi.ro.mounted);
	info->version = fi->sbi.rw.other.version;
	info->logged = fi->sbi.rw.other.logged;
	info->maxsize = fi->sbi.rw.other.max_length;
	info->size = fi->sbi.rw.read.data_length;
	info->space = fi->sbi.ro.data_space;
	info->devsize = fi->sbi.ro.device_size;
	info->start = fi->sbi.rw.read.data_start;
	info->commit_size = fi->sbi.rw.other.commit_count[0];
	info->commit_time = fi->sbi.rw.other.commit_count[1];
	info->commit_forced = fi->sbi.rw.other.commit_count[2];
	info->flags = fi->sbi.ro.flags;
	info->nsuper = fi->sbi.ro.num_superblocks;
	info->align = fi->sbi.ro.log_alignment;
	strcpy(info->fs, fi->options.fspath);
	mutex_unlock(&fi->sbi.mutex);
	return 0;
}

struct file_operations shall_proc_info = {
	.open		= info_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= seq_release_private,
};

/* common code for logs and hlog files */

static inline int is_any_open(const struct shall_fsinfo *fi) {
	return atomic_read(&fi->sbi.ro.logs_reading) ||
	       atomic_read(&fi->sbi.ro.logs_writing);
}

void shall_notify_umount(struct shall_fsinfo *fi) {
	int retry = 10;
	/* we need to tell people to close the files... the wake_up
	 * says "data available" but it really means "end-of-file" */
	atomic_set(&fi->sbi.ro.logs_valid, 0);
	wake_up_all(&fi->sbi.ro.data_queue);
	/* now we wait until they've all self-destructed, otherwise they may
	 * end up accessing the superblock info after it has been freed;
	 * we sleep rather than using yet another wait queue; anyway we
	 * don't plan to keep umounting over and over again, do we? */
	while (is_any_open(fi) && retry-- > 0)
		schedule_timeout_interruptible(HZ / 10);
}

static ssize_t xlog_read(struct file *file, char __user *buf,
			 size_t count, loff_t *pos)
{
	struct shall_proc_user * li = file->private_data;
	struct shall_fsinfo * fi = li->fi;
	ssize_t ret;
	/* if the filesystem was unmounted, return end-of-file */
	if (! atomic_read(&fi->sbi.ro.logs_valid)) return 0;
	ret = li->get(fi, buf, count);
	if (ret != 0) return ret;
	if (file->f_flags & O_NONBLOCK)
		return -EAGAIN;
	while (ret == 0) {
		atomic_set(&fi->sbi.ro.some_data, 0);
		/* we sleep until one of the following happens:
		 *     there is data
		 *     the filesystem is unmounted
		 *     the file is closed */
#define has_data(fi) \
		(atomic_read(&(fi)->sbi.ro.some_data) || \
		 ! atomic_read(&(fi)->sbi.ro.logs_valid) || \
		 ! atomic_read(&(fi)->sbi.ro.logs_reading))
		if (wait_event_interruptible(fi->sbi.ro.data_queue,
					     has_data(fi)))
			return -EINTR;
#undef has_data
		/* quickly read here before checking for end-of-file, as
		 * this will return the unmount log if it fits; however
		 * this must not block as the umount may be waiting... */
		ret = li->get(fi, buf, count);
		/* might have started an umount while we waited... check */
		if (! atomic_read(&fi->sbi.ro.logs_valid)) return ret;
		/* might also have closed the file... */
		if (! atomic_read(&fi->sbi.ro.logs_reading)) return ret;
	}
	return ret;
}

static int xlog_open(struct inode *inode, struct file *file, get_logs_t func) {
	struct shall_fsinfo * fi;
	struct shall_proc_user * li;
	if (file->f_mode & (func ? FMODE_WRITE : FMODE_READ))
		return -EPERM;
	/* then determine which device we need to use */
	fi = proc_get_parent_data(inode);
	if (! fi) return -ENOENT;
	if (! atomic_read(&fi->sbi.ro.logs_valid)) return -ENOENT;
	if (func) {
		/* blog/hlog are exclusive, if you open one, you can't have
		 * another; we now atomically test logs_open while setting it */
		if (atomic_xchg(&fi->sbi.ro.logs_reading, 1)) return -EBUSY;
	} else {
		/* ctrl can be open as many times as you want... but we must
		 * count how many times it is open so the unmount can wait
		 * for them */
		atomic_inc(&fi->sbi.ro.logs_writing);
	}
	/* OK, we have the file, store the data for later use */
	li = shall_kmalloc(fi, sizeof(*li), GFP_KERNEL);
	if (! li) return -ENOMEM;
	li->get = func;
	li->fi = fi;
	file->private_data = li;
	return 0;
}

static unsigned int xlog_poll(struct file *file, poll_table *poll) {
	struct shall_proc_user * li = file->private_data;
	struct shall_fsinfo * fi = li->fi;
	unsigned int ret = 0;
	if (! atomic_read(&fi->sbi.ro.logs_valid))
		ret = POLLHUP | POLLRDHUP;
	else if (atomic_read(&fi->sbi.ro.some_data))
		ret = POLLIN | POLLRDNORM;
	return ret;
}

static int xlog_release(struct inode *inode, struct file *file) {
	struct shall_proc_user * li;
	li = file->private_data;
	file->private_data = NULL;
	if (! li) return 0;
	if (li->get)
		atomic_set(&li->fi->sbi.ro.logs_reading, 0);
	else
		atomic_dec(&li->fi->sbi.ro.logs_writing);
	if (atomic_read(&li->fi->sbi.ro.logs_valid))
		shall_kfree(li->fi, li);
	else
		kfree(li);
	return 0;
}

/* code specific to blog files */

static int blog_open(struct inode *inode, struct file *file) {
	return xlog_open(inode, file, shall_bin_logs);
}

struct file_operations shall_proc_blog = {
	.open		= blog_open,
	.read		= xlog_read,
	.llseek		= no_llseek,
	.release	= xlog_release,
	.poll		= xlog_poll,
};

/* code specific to hlog files */

#ifdef CONFIG_SHALL_FS_DEBUG
static int hlog_open(struct inode *inode, struct file *file) {
	return xlog_open(inode, file, shall_print_logs);
}

struct file_operations shall_proc_hlog = {
	.open		= hlog_open,
	.read		= xlog_read,
	.llseek		= no_llseek,
	.release	= xlog_release,
	.poll		= xlog_poll,
};
#endif

/* special file to issue control commands */

static int ctrl_open(struct inode *inode, struct file *file) {
	return xlog_open(inode, file, NULL);
}

static unsigned int ctrl_poll(struct file *file, poll_table *poll) {
	struct shall_proc_user * li = file->private_data;
	return atomic_read(&li->fi->sbi.ro.logs_valid)
	     ? POLLOUT | POLLWRNORM
	     : POLLHUP;
}

static ssize_t ctrl_write(struct file *file, const char __user *buf,
			  size_t count, loff_t *pos)
{
	char copy[144];
	struct shall_proc_user * li = file->private_data;
	struct shall_fsinfo * fi = li->fi;
	size_t done = 0, err = 0;
	while (count > 0) {
		size_t eptr, todo = count - done;
		if (! atomic_read(&fi->sbi.ro.logs_valid)) {
			err = -EPIPE;
			goto error;
		}
		if (todo >= sizeof(copy)) todo = sizeof(copy) - 1;
		if (copy_from_user(copy, buf + done, todo)) {
			err = -EFAULT;
			goto error;
		}
		/* find line ending if any, and then skip past all newlines */
		eptr = 0;
		while (eptr < count && copy[eptr] != '\n') eptr++;
		if (eptr >= count) {
			err = -EINVAL;
			goto error;
		}
		copy[eptr++] = 0;
		/* accept an empty line but skip the locking */
		if (! copy[0]) goto do_nothing;
		/* now acquire a lock before processing things... */
		mutex_lock(&fi->sbi.mutex);
		/* they may have started an umount while we acquired the lock */
		if (! atomic_read(&fi->sbi.ro.logs_valid)) {
			err = -EPIPE;
			goto error_unlock;
		}
		if (strncmp(copy, "commit", 6) == 0) {
			shall_write_data(fi, 1, 2, 1);
			goto unlock;
		}
		if (strncmp(copy, "clear", 5) == 0) {
			int discard;
			if (sscanf(copy + 5, "%d", &discard) < 1) {
				err = -EINVAL;
				goto error_unlock;
			}
			if (discard < 0) {
				err = -ERANGE;
				goto error_unlock;
			}
			if (discard == 0) goto unlock;
			err = shall_delete_logs(fi, discard);
			if (err < 0) {
				if (done > eptr)
					done -= eptr;
				else
					done = err;
			}
			goto unlock;
		}
		if (strncmp(copy, "userlog", 7) == 0) {
		    const char * data = copy + 7;
		    if (*data && isspace(*data)) data++;
		    shall_log_1n(fi, SHALL_USERLOG, data, 0);
		    goto unlock;
		}
		/* invalid command */
		err = -EINVAL;
		goto error_unlock;
	unlock:
		mutex_unlock(&fi->sbi.mutex);
	do_nothing:
		done += eptr;
	}
	return done;
error_unlock:
	mutex_unlock(&fi->sbi.mutex);
error:
	return done ? done : err;
}

struct file_operations shall_proc_ctrl = {
	.open		= ctrl_open,
	.write		= ctrl_write,
	.llseek		= no_llseek,
	.release	= xlog_release,
	.poll		= ctrl_poll,
};

