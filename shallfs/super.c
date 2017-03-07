/*
 *  linux/fs/shallfs/super.c
 *
 *  some code inspired by various Linux filesystems, particularly overlyfs,
 *  which is Copyright (C) 2011 Novell Inc (GPL).
 */

#include <linux/module.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/statfs.h>
#include <linux/seq_file.h>
#include <linux/time.h>
#include <linux/ctype.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/wait.h>
#include <linux/buffer_head.h>
#include <linux/crc32.h>
#include <linux/kthread.h>
#include <linux/jiffies.h>
#include <linux/proc_fs.h>
#include <linux/posix_acl_xattr.h>
#include <linux/atomic.h>
#include <linux/mount.h>
#include <linux/blkdev.h>
#include <linux/posix_acl.h>
#include <shallfs/operation.h>
#include <shallfs/device.h>
#include "shallfs.h"
#include "inode.h"
#include "log.h"
#include "proc.h"
#include "device.h"
#include "super.h"

#define SHALL_SB_MAGIC "SHALL 01"
#define SHALL_MAGIC 0x304c4853

/* see replace_commit_buffer() and shall_remount() */
struct commit_replace {
	struct shall_options options;
	struct shall_fsinfo * fi;
	int err;
#ifdef CONFIG_SHALL_FS_DEBUG
	void * old_commit;
	void * old_data;
#endif
};

/* tables used to store flags and values; they are used by parse_options()
 * and show_options() */
struct flags_value {
	enum shall_flags value;
	const char * name;
};

struct flags_table {
	enum shall_flags mask;
	int n_values;
	const struct flags_value * values;
};

static const struct flags_value overflow_values[] = {
	{ OVERFLOW_DROP,	"drop" },
	{ OVERFLOW_WAIT,	"wait" },
};

static const struct flags_table overflow_table = {
	.mask		= OVERFLOW_MASK,
	.n_values	= sizeof(overflow_values) / sizeof(overflow_values[0]),
	.values		= overflow_values,
};

static const struct flags_value too_big_values[] = {
	{ TOO_BIG_LOG,		"log" },
	{ TOO_BIG_ERROR,	"error" },
};

static const struct flags_table too_big_table = {
	.mask		= TOO_BIG_MASK,
	.n_values	= sizeof(too_big_values) / sizeof(too_big_values[0]),
	.values		= too_big_values,
};

static const struct flags_value log_values[] = {
	{ LOG_BEFORE,		"before" },
	{ LOG_AFTER,		"after" },
	{ LOG_TWICE,		"twice" },
	{ LOG_TWICE,		"both" },
};

static const struct flags_table log_table = {
	.mask		= LOG_MASK,
	.n_values	= sizeof(log_values) / sizeof(log_values[0]),
	.values		= log_values,
};

#ifdef CONFIG_SHALL_FS_DEBUG
static const struct flags_value debug_values[] = {
	{ DEBUG_OFF, 		"off" },
	{ DEBUG_OFF, 		"false" },
	{ DEBUG_OFF, 		"no" },
	{ DEBUG_ON,  		"on" },
	{ DEBUG_ON,  		"true" },
	{ DEBUG_ON,  		"yes" },
};

static const struct flags_table debug_table = {
	.mask		= DEBUG_MASK,
	.n_values	= sizeof(debug_values) / sizeof(debug_values[0]),
	.values		= debug_values,
};

static const struct flags_value name_values[] = {
	{ NAME_OFF, 		"off" },
	{ NAME_OFF, 		"false" },
	{ NAME_OFF, 		"no" },
	{ NAME_ON,  		"on" },
	{ NAME_ON,  		"true" },
	{ NAME_ON,  		"yes" },
};

static const struct flags_table name_table = {
	.mask		= NAME_MASK,
	.n_values	= sizeof(debug_values) / sizeof(debug_values[0]),
	.values		= debug_values,
};
#endif

/* default options */
static const struct shall_options default_options = {
	.fspath		= NULL,
	.pathfilter	= NULL,
	.commit_seconds	= 5,
	.commit_size	= PAGE_SIZE,
	.data		= NULL,
	.flags		= OVERFLOW_WAIT | TOO_BIG_LOG | LOG_AFTER
#ifdef CONFIG_SHALL_FS_DEBUG
			| DEBUG_OFF | NAME_ON
#endif
};

/* for /proc/fs/shallfs */
static struct proc_dir_entry * fs_proc;
struct shall_fsinfo * shall_fs_list;
struct mutex shall_fs_mutex;

/* check if the next mount option starts with key= -> if not, return 0;
 * if it does, decode the value after the "=" and set *val to the start
 * of the decoded value, *vlen to the length (excluding terminating NUL),
 * then return 1 */
static int set_string(char *data, int len, const char *key,
		      char **val, int *vlen)
{
	int klen = strlen(key), s, d;
	if (klen >= len || data[klen] != '=') return 0;
	if (strncmp(data, key, klen) != 0) return 0;
	klen++;
	data += klen;
	len -= klen;
	*val = data;
	/* see if we need to unescape anything */
	for (s = d = 0; s < len; d++, s++) {
		if (data[s] == '\\' && s + 1 < len) s++;
		data[d] = data[s];
	}
	if (vlen) *vlen = d;
	data[d] = 0;
	return 1;
}

/* similar to set_string, however it takes a colon-separated list and
 * sets *vcount to the number of strings, *vlen to the total length of
 * all strings (including terminating NULs) and *val to the start of
 * the first value: all other values just follow immediately */
static int set_pathlist(char *data, int len, const char *key,
		        char **val, int *vlen, int *vcount)
{
	int klen = strlen(key), s, d, count;
	if (klen >= len || data[klen] != '=') return 0;
	if (strncmp(data, key, klen) != 0) return 0;
	klen++;
	data += klen;
	len -= klen;
	*val = data;
	count = 0;
	/* split into components, and unescape as we go along */
	for (s = d = 0; s < len; d++, s++) {
		if (data[s] == ':') {
			data[d] = 0;
			count++;
			continue;
		}
		if (data[s] == '\\' && s + 1 < len) s++;
		data[d] = data[s];
	}
	data[d++] = 0;
	if (vlen) *vlen = d;
	if (vcount) *vcount = count;
	return 1;
}

/* check if the next mount option starts with key= (if not, return 0);
 * then check that the value is one of the flag values provided in the
 * table (if not, set *ok to 0 and return 1), and finally update the
 * *flag argument by first masking off all possible values for "this"
 * flag and then adding the appropriate bits; then return 1 */
static int set_flag(char *data, int len, const char *key,
		    const struct flags_table *table,
		    enum shall_flags *flags, int *ok)
{
	char * vp;
	int i;
	if (! set_string(data, len, key, &vp, NULL)) return 0;
	for (i = 0; i < table->n_values; i++) {
		if (strcmp(table->values[i].name, vp) != 0) continue;
		*flags &= ~table->mask;
		*flags |= table->values[i].value;
		return 1;
	}
	printk(KERN_ERR "Invalid value for \"%s\": \"%s\"\n", key, vp);
	*ok = 0;
	return 1;
}

/* move *data to point to the mount option after this one: and update
 * the string so that this option ends with a NUL (by replacing the comma
 * if necessary); an escaped comma is left in place as it is not an
 * option terminator */
static char * next_option(char **data) {
	char * src = *data, *start = *data;
	if (! *src) return NULL;
	while (*src) {
		if (*src == ',') {
			*src++ = 0;
			break;
		}
		if (*src == '\\') {
			src++;
			if (! *src) break;
		}
		src++;
	}
	*data = src;
	return start;
}

/* parse (re)mount options; the caller initialises *opts to the default
 * options (for mount) or the previous options (for remount) and this will
 * make changes as requested by the userspace */
static int parse_options(char *data, struct shall_options *opts)
{
	char * fs = NULL, * filt = NULL, * ptr;
	int fslen = 0, filtlen = 0, filtcount = 0, ok = 1, len;
	while ((ptr = next_option(&data)) != NULL) {
		char * vp;
		int len = strlen(ptr);
		if (set_string(ptr, len, "fs", &fs, &fslen))
			continue;
		if (set_pathlist(ptr, len, "pathfilter",
				 &filt, &filtlen, &filtcount))
		{
			/* check that patterns are valid: at present, it
			 * means that a directory can be replaced with a
			 * single "*" but no other globs are permitted;
			 * also, no "." and ".." components and no empty
			 * components or slash at end; note that we do
			 * not have a mechanism to escape a "*" for now */
			const char * p = filt;
			int c;
			for (c = 0; c < filtcount; c++, p += 1 + strlen(p)) {
				int i;
				for (i = 0; p[i]; i++) {
					if (p[i] == '*') {
						if (i > 0 && p[i - 1] != '/')
							goto invalid_pattern;
						if (p[i + 1] && p[i + 1] != '/')
							goto invalid_pattern;
					}
					if (p[i] == '/') {
						/* check for empty component */
						if (! p[i + 1])
							goto invalid_pattern;
						if (p[i + 1] == '/')
							goto invalid_pattern;
						/* check for "." */
						if (p[i + 1] != '.')
							continue;
						if (! p[i + 2])
							goto invalid_pattern;
						if (p[i + 2] == '/')
							goto invalid_pattern;
						/* check for ".." */
						if (p[i + 2] != '.')
							continue;
						if (! p[i + 3])
							goto invalid_pattern;
						if (p[i + 3] == '/')
							goto invalid_pattern;
					}
					continue;
				invalid_pattern:
					printk(KERN_ERR
					       "Invalid path filter \"%s\"\n",
					       p);
					ok = 0;
				}
			}
			continue;
		}
		if (set_flag(ptr, len, "overflow", &overflow_table,
			     &opts->flags, &ok))
			continue;
		if (set_flag(ptr, len, "too_big", &too_big_table,
			     &opts->flags, &ok))
			continue;
		if (set_flag(ptr, len, "log", &log_table,
			     &opts->flags, &ok))
			continue;
#ifdef CONFIG_SHALL_FS_DEBUG
		if (set_flag(ptr, len, "debug", &debug_table,
			     &opts->flags, &ok))
			continue;
		if (set_flag(ptr, len, "name", &name_table,
			     &opts->flags, &ok))
			continue;
#endif
		if (set_string(ptr, len, "commit", &vp, NULL)) {
			int seconds, size;
			if (sscanf(vp, "%d:%d", &seconds, &size) != 2 ||
			    seconds < 1 ||
			    size < PAGE_SIZE)
			{
				printk(KERN_ERR
				       "Invalid value %s for commit (%d, %d)\n",
				       vp, seconds, size);
				ok = 0;
				continue;
			}
			opts->commit_seconds = seconds;
			opts->commit_size = size;
			continue;
		}
		printk(KERN_ERR "Invalid mount option %s\n", ptr);
		ok = 0;
	}
	if (! ok) return -EINVAL;
	/* now replace the strings with any new ones */
	len = 0;
	if (! fs) {
		fs = opts->fspath;
		if (fs) fslen = strlen(fs);
	}
	if (fs) len += 1 + fslen;
	if (! filt) {
		int c;
		const char * pf;
		pf = filt = opts->pathfilter;
		filtcount = filtlen = 0;
		if (pf) {
			for (c = 0; c < opts->pathfilter_count; c++) {
				int l = 1 + strlen(pf);
				filtlen += l;
				pf += l;
			}
			filtcount = opts->pathfilter_count;
		}
	}
	if (filt) len += filtlen;
	ptr = opts->data = kmalloc(len, GFP_KERNEL);
	if (! ptr)
		return -ENOMEM;
	if (fs) {
		opts->fspath = ptr;
		strncpy(ptr, fs, fslen);
		ptr[fslen] = 0;
		ptr += 1 + fslen;
	} else {
		opts->fspath = NULL;
	}
	if (filt) {
		int c;
		opts->pathfilter = ptr;
		opts->pathfilter_count = filtcount;
		for (c = 0; c < filtcount; c++) {
			int l = 1 + strlen(filt);
			strcpy(ptr, filt);
			filt += l;
			ptr += l;
		}
	} else {
		opts->pathfilter = NULL;
		opts->pathfilter_count = 0;
	}
	return 0;
}

/* update a number of superblocks; this is only used during mount and umount */
static int shall_update_superblock(struct shall_fsinfo *fi) {
	int i, nrecs, diff, which;
	fi->sbi.rw.other.version++;
	nrecs = 7;
	if (nrecs > fi->sbi.ro.num_superblocks)
		nrecs = fi->sbi.ro.num_superblocks;
	diff = fi->sbi.ro.num_superblocks / nrecs;
	which = 0;
	for (i = 0; i < nrecs; i++) {
		int err = shall_write_superblock(fi, which, 1);
		if (err < 0) return err;
		which += diff;
		if (which >= fi->sbi.ro.num_superblocks)
			which -= fi->sbi.ro.num_superblocks;
	}
	fi->sbi.rw.other.last_sb_written = which;
	return 0;
}

/* this is called during umount; all we need to do is commit the logs
 * to journal and free any memory we allocated */
static void shall_put_super(struct super_block *sb) {
	struct shall_fsinfo * fi = (struct shall_fsinfo *)sb->s_fs_info;
	/* first log that we are unmounting; this must be done before
	 * we clear allow_commit_thread or it will deadlock; actually,
	 * just in case, we set it here */
	atomic_set(&fi->sbi.ro.allow_commit_thread, 1);
#ifdef CONFIG_SHALL_FS_DEBUG
	if (IS_DEBUG(fi)) {
		char buf[32];
		snprintf(buf, sizeof(buf), "vfree(%p)",
			 fi->sbi.rw.other.commit_buffer);
		shall_log_debug(fi, buf);
		snprintf(buf, sizeof(buf), "kfree(%p)", fi->options.data);
		shall_log_debug(fi, buf);
		snprintf(buf, sizeof(buf), "kfree(%p)", fi);
		shall_log_debug(fi, buf);
	}
#endif
	shall_log_0n(fi, -SHALL_UMOUNT, 0);
	/* make sure all log readers are notified, and they will get an
	 * end-of-file condition */
	shall_notify_umount(fi);
	/* shall_commit_logs has the side effect of waiting for the commit
	 * thread to complete the current run, and it won't let it start
	 * a new run if it was called with allow_commit_thread == 0; and
	 * as soon as it returns we kill the commit thread and that will
	 * be it */
	atomic_set(&fi->sbi.ro.allow_commit_thread, 0);
	shall_commit_logs(fi, NULL, NULL);
	if (atomic_read(&fi->sbi.ro.thread_running))
		kthread_stop(fi->commit_thread);
	/* remove directory /proc/fs/shallfs/<device> */
	proc_remove(fi->proc);
	/* mark superblock clean and update a few */
	fi->sbi.ro.flags &= ~SHALL_SB_DIRTY;
	/* ignore error from shall_update_superblock because we can't
	 * report it; but a problem will result in a complaint when we
	 * try to mount it next time (and there'll be an error in dmesg) */
	shall_update_superblock(fi);
	/* apparently, we don't need to destroy fi->log_queue because
	 * there won't be anybody waiting - if they were, we wouldn't
	 * be able to umount... well, that's the theory and the kernel
	 * will make sure it happens, here, we need to just trust it;
	 * I personally would be happier if I could check */
	if (fi->sbi.rw.other.commit_buffer)
		vfree(fi->sbi.rw.other.commit_buffer);
	if (fi->options.data) kfree(fi->options.data);
	mntput(fi->mount);
	path_put(&fi->root_path);
	sync_blockdev(sb->s_bdev);
	invalidate_bdev(sb->s_bdev);
	mutex_lock(&shall_fs_mutex);
	if (fi->prev)
		fi->prev->next = fi->next;
	else
		shall_fs_list = fi->next;
	if (fi->next)
		fi->next->prev = fi->prev;
	mutex_unlock(&shall_fs_mutex);
	kfree(fi);
	sb->s_fs_info = NULL;
}

/* commit everything and write out a new superblock */
static int shall_sync_fs(struct super_block *sb, int wait) {
	struct timespec now;
	struct shall_fsinfo * fi = (struct shall_fsinfo *)sb->s_fs_info;
	int n_sb, err1, err2;
	mutex_lock(&fi->sbi.mutex);
	err1 = shall_write_data(fi, 1, 2, 1);
	now = current_fs_time(fi->sb);
	n_sb = ++fi->sbi.rw.other.last_sb_written;
	fi->sbi.rw.other.last_commit = now.tv_sec;
	fi->sbi.rw.other.version++;
	err2 = shall_write_superblock(fi, n_sb, 0);
	if (wait)
		err2 = blkdev_issue_flush(sb->s_bdev, GFP_KERNEL, NULL);
	mutex_unlock(&fi->sbi.mutex);
	return err1 < 0 ? err1 : err2;
}

/* LVM is about to take a snapshot, so we mark FS clean and commit all;
 * this is very similar to shall_sync_fs(sb, 1) but we always update
 * superblock #0 in addition to the "next" one; note that we must rely
 * on the caller to stop further updates until the unfreeze */
static int shall_freeze_fs(struct super_block *sb) {
	struct timespec now;
	struct shall_fsinfo * fi = (struct shall_fsinfo *)sb->s_fs_info;
	int n_sb, err1, err2;
	mutex_lock(&fi->sbi.mutex);
	err1 = shall_write_data(fi, 1, 2, 1);
	now = current_fs_time(fi->sb);
	n_sb = fi->sbi.rw.other.last_sb_written;
	fi->sbi.rw.other.last_sb_written = 0;
	fi->sbi.rw.other.last_commit = now.tv_sec;
	fi->sbi.rw.other.version++;
	fi->sbi.ro.flags &= ~SHALL_SB_DIRTY;
	shall_write_superblock(fi, n_sb, 0);
	shall_write_superblock(fi, 0, 0);
	err2 = blkdev_issue_flush(sb->s_bdev, GFP_KERNEL, NULL);
	mutex_unlock(&fi->sbi.mutex);
	return err1 < 0 ? err1 : err2;
}

/* LVM has taken the snapshot, so we continue normal operation; we must
 * update the superblock again, marking it dirty; however no need to
 * commit anything as the caller guaratees that nothing else happened
 * between freeze and unfreeze */
static int shall_unfreeze_fs(struct super_block *sb) {
	struct timespec now;
	struct shall_fsinfo * fi = (struct shall_fsinfo *)sb->s_fs_info;
	int err;
	mutex_lock(&fi->sbi.mutex);
	now = current_fs_time(fi->sb);
	fi->sbi.rw.other.last_sb_written = 1;
	fi->sbi.rw.other.last_commit = now.tv_sec;
	fi->sbi.rw.other.version++;
	fi->sbi.ro.flags |= SHALL_SB_DIRTY;
	shall_write_superblock(fi, 0, 0);
	shall_write_superblock(fi, 1, 0);
	err = blkdev_issue_flush(sb->s_bdev, GFP_KERNEL, NULL);
	mutex_unlock(&fi->sbi.mutex);
	return err;
}

/* update current mount options; must make sure we don't confuse the
 * commit thread; see comments in shall_remount() */
static void new_options(void *_cr) {
	struct commit_replace * cr = _cr;
	if (cr->fi->options.commit_size != cr->options.commit_size) {
		char * buffer;
		if (cr->fi->sbi.rw.other.commit_buffer) {
#ifdef CONFIG_SHALL_FS_DEBUG
			cr->old_commit = cr->fi->sbi.rw.other.commit_buffer;
#endif
			vfree(cr->fi->sbi.rw.other.commit_buffer);
			cr->fi->sbi.rw.other.commit_buffer = NULL;
		}
		buffer = shall_vmalloc(cr->fi, cr->options.commit_size);
		if (! buffer) {
			cr->err = -ENOMEM;
			return;
		}
		cr->fi->sbi.rw.other.commit_buffer = buffer;
	}
	if (cr->fi->options.data && cr->fi->options.data != cr->options.data) {
#ifdef CONFIG_SHALL_FS_DEBUG
		cr->old_data = cr->fi->options.data;
#endif
		kfree(cr->fi->options.data);
	}
	cr->fi->options = cr->options;
	cr->err = 0;
}

static int shall_remount(struct super_block *sb, int *flags, char *data) {
	struct commit_replace cr;
	struct shall_fsinfo * fi = (struct shall_fsinfo *)sb->s_fs_info;
	char * datacopy;
	int err = 0, wake_them = 0;
	if (IS_LOG_BEFORE(fi)) {
		err = shall_log_1n(fi, -SHALL_REMOUNT, data, 0);
		if (err) return err;
	}
	/* make sure we don't update fi until we call shall_commit_logs;
	 * so we copy the mount options, update the copy, and then call
	 * shall_commit_logs to update fi; we don't lock the options to
	 * copy them as they can only be changed on remount anyway */
	cr.options = fi->options;
	datacopy = kstrdup(data, GFP_KERNEL);
	if (! datacopy) {
	    err = -ENOMEM;
	    goto out_log;
	}
	err = parse_options(datacopy, &cr.options);
	kfree(datacopy);
	if (err) goto out_log;
	/* fspath can only change on a remount, so it's safe to access it
	 * here without holding the lock */
	if (fi->options.fspath && cr.options.fspath) {
		if (strcmp(fi->options.fspath, cr.options.fspath) != 0) {
			printk(KERN_ERR "Cannot change fs= on remount\n");
			err = -EINVAL;
			goto out_freedata;
		}
	} else {
		printk(KERN_ERR "Internal error, fspath was NULL?\n");
		err = -EINVAL;
		goto out_freedata;
	}
	/* changing overflow=wait to overflow=drop means we'll have to
	 * wake processes up later to deal with it; we are accessing
	 * fi->options.flags without holding the lock, but as these can
	 * only change on remount we'll be safe doing so */
	if (IS_WAIT(fi) && IS_DROP_O(cr.options))
		wake_them = 1;
	/* if we changed mount options in such a way that the commit thread
	 * may get confused... better make sure it doesn't!  The safest way to
	 * do so is to make the changes inside shall_commit_logs which calls
	 * things as required while holding the appropriate lock; if no such
	 * change happened, we could just update the options, but anyway, we
	 * don't expect to keep remounting all the time so we'll stick to the
	 * safe option */
	cr.fi = fi;
	cr.err = 0;
#ifdef CONFIG_SHALL_FS_DEBUG
	cr.old_commit = NULL;
	cr.old_data = NULL;
#endif
	shall_commit_logs(fi, new_options, &cr);
	if (cr.err) {
		err = cr.err;
		goto out_log;
	}
#ifdef CONFIG_SHALL_FS_DEBUG
	if (IS_DEBUG(fi)) {
		if (cr.old_commit) {
			char buf[32];
			snprintf(buf, sizeof(buf), "kfree(%p)",
				 cr.old_commit);
			shall_log_debug(fi, buf);
			snprintf(buf, sizeof(buf), "vmalloc(%d)=%p",
				 fi->options.commit_size,
				 fi->sbi.rw.other.commit_buffer);
			shall_log_debug(fi, buf);
		}
		if (cr.old_data) {
			char buf[32];
			snprintf(buf, sizeof(buf), "kfree(%p)",
				 cr.old_data);
			shall_log_debug(fi, buf);
			snprintf(buf, sizeof(buf), "kmalloc(?)=%p",
				 fi->options.data);
			shall_log_debug(fi, buf);
		}
	}
#endif
	/* if we changed overflow=wait to overflow=drop we need to wake
	 * up any process waiting for space, which will then just drop
	 * the extra logs */
printk(KERN_ERR "About to wake things up (wake_them=%d)\n", wake_them); // XXX
	if (wake_them)
		wake_up_all(&fi->lq.log_queue);
out_freedata:
	/* we get here if we encounter an error before updating fi->options,
	 * so free cr.options.data if not NULL and different from
	 * fi->options.data; fi->options.data can only change on a remount,
	 * so it's safe to access it here without holding the lock */
	if (cr.options.data && fi->options.data != cr.options.data)
		kfree(cr.options.data);
out_log:
	if (IS_LOG_AFTER(fi))
		shall_log_1n(fi, SHALL_REMOUNT, data, err);
	return err;
}

/* this is the opposite of set_flag above... we are reconstructing the
 * current mount options to show in /proc/mounts, and this adds a single
 * ,flag=value (e.g. ,overflow=wait) */
static void add_flag(struct seq_file *m, const char *key,
		     const struct flags_table *table,
		     enum shall_flags flags)
{
	const char * value = "?";
	int i;
	flags &= table->mask;
	for (i = 0; i < table->n_values; i++) {
		if (table->values[i].value == flags) {
			value = table->values[i].name;
			break;
		}
	}
	seq_printf(m, ",%s=%s", key, value);
}

/* add a single string value to the mount option as ,key=value; we need
 * to escape commas and backspaces */
static void add_string(struct seq_file *m, const char *key,
		       const char * value)
{
	/* escape any comma or backslash */
	seq_printf(m, ",%s=", key);
	while (*value) {
		int len;
		for (len = 0;
		    value[len] && value[len] != ',' && value[len] != '\\';
		    len++) ;
		seq_printf(m, "%.*s", len, value);
		value += len;
		if (*value) seq_printf(m, "\\%c", *value++);
	}
}

/* add a list of strings value to the mount option as ,key=value[:value]...;
 * we need to escape commas, colons and backspaces */
static void add_pathlist(struct seq_file *m, const char *key,
			 const char *value, int count)
{
	int c;
	seq_printf(m, ",%s=", key);
	for (c = 0; c < count; c++) {
		if (c) seq_printf(m, ":");
		while (*value) {
			int len;
			for (len = 0;
			    value[len] &&
				value[len] != ',' &&
				value[len] != ':' &&
				value[len] != '\\';
			    len++) ;
			seq_printf(m, "%.*s", len, value);
			value += len;
			if (*value) seq_printf(m, "\\%c", *value++);
		}
		value++;
	}
}

/* this is called by /proc/mounts to show the current mount options,
 * so we just add one after another */
static int shall_show_options(struct seq_file *m, struct dentry *dentry) {
	struct super_block *sb = dentry->d_sb;
	struct shall_fsinfo * fi = (struct shall_fsinfo *)sb->s_fs_info;
	add_string(m, "fs", fi->options.fspath);
	add_flag(m, "overflow", &overflow_table, fi->options.flags);
	add_flag(m, "too_big", &too_big_table, fi->options.flags);
	seq_printf(m, ",commit=%d,%d",
		   fi->options.commit_seconds, fi->options.commit_size);
	add_flag(m, "log", &log_table, fi->options.flags);
	if (fi->options.pathfilter)
		add_pathlist(m, "pathfilter", fi->options.pathfilter,
			     fi->options.pathfilter_count);
#ifdef CONFIG_SHALL_FS_DEBUG
	add_flag(m, "debug", &debug_table, fi->options.flags);
	add_flag(m, "name", &name_table, fi->options.flags);
#endif
	return 0;
}

/* userspace called statfs() on us; we just delegate this to the
 * underlying filesystem... except that we have a different file
 * system type */
static int shall_statfs(struct dentry *dentry, struct kstatfs *buf) {
	struct shall_fsinfo * fi = dentry->d_sb->s_fs_info;
	int err = vfs_statfs(&fi->root_path, buf);
	if (! err)
		buf->f_type = SHALL_MAGIC;
	return err;
}

/* superblock operations, we only care about a small number of them, and
 * the rest is fine as either kernel's default or ENOSYS */
static struct super_operations shall_super_ops = {
	.put_super	= shall_put_super,
	.statfs		= shall_statfs,
	.sync_fs	= shall_sync_fs,
	.freeze_fs	= shall_freeze_fs,
	.unfreeze_fs	= shall_unfreeze_fs,
	.remount_fs	= shall_remount,
	.show_options	= shall_show_options,
	.evict_inode	= shall_evict_inode,
};

/* look for a valid superblock; we only do this if the first superblock is
 * not valid; we don't know how many superblocks there are, but we do know
 * that they cannot be past the end of device... */
static int search_superblock(struct shall_fsinfo *fi) {
	sector_t limit = fi->sb->s_bdev->bd_inode->i_size / SHALL_DEV_BLOCK;
	int n = 0;
	printk(KERN_INFO "Looking for an alternative superblock");
	while (1) {
		n++;
		if (shall_superblock_location(n) >= limit) {
			printk(KERN_ERR "Could not find a valid superblock");
			return -EINVAL;
		}
		if (shall_read_superblock(fi, n, 1) >= 0)
			return 0;
	}
}

/* read all superblocks and find the "best" one */
static void scan_all_superblocks(struct shall_fsinfo *fi) {
	struct shall_sbinfo sbi = fi->sbi;
	int n;
	for (n = 0; n < sbi.ro.num_superblocks; n++) {
		if (shall_read_superblock(fi, n, 1) < 0) continue;
		if (fi->sbi.rw.other.version <= sbi.rw.other.version) continue;
		sbi = fi->sbi;
	}
	fi->sbi = sbi;
}

static int create_proc_entries(struct shall_fsinfo *fi) {
	char devid[32];
	snprintf(devid, sizeof(devid), "%x:%x",
		 MAJOR(fi->sb->s_dev), MINOR(fi->sb->s_dev));
	fi->proc = proc_mkdir_data(devid, 0755, fs_proc, fi);
	if (! fi->proc) {
		printk(KERN_ERR "Cannot create /proc/fs/shallfs/%s\n", devid);
		return -ENOENT;
	}
	if (! proc_create("info", 0400, fi->proc, &shall_proc_info) ||
	    ! proc_create("blog", 0400, fi->proc, &shall_proc_blog) ||
#ifdef CONFIG_SHALL_FS_DEBUG
	    ! proc_create("hlog", 0400, fi->proc, &shall_proc_hlog) ||
#endif
	    ! proc_create("ctrl", 0200, fi->proc, &shall_proc_ctrl))
	{
		printk(KERN_ERR "Cannot create control entry for filesystem\n");
		proc_remove(fi->proc);
		return -ENOENT;
	}
	return 0;
}

/* fake xattr handler which passes everything except the POSIX ACL xattrs
 * to our functions; this means that we can use generic handlers and still
 * intercept stuff we need to log; the ACL xattrs will be intercepted by
 * the set_acl call instead */
static const struct xattr_handler *sb_xattr_handler[] = {
#ifdef CONFIG_EXT4_FS_POSIX_ACL
	&posix_acl_access_xattr_handler,
	&posix_acl_default_xattr_handler,
#endif
	&shall_xattr_handler,
	NULL
};

/* this is called when mounting, and does a lot of stuff including reading
 * the journal device, allocating buffers, etc */
static int shall_fill_super(struct super_block *sb, void *data, int silent) {
	struct timespec now;
	const struct dentry_operations * dop;
	const struct inode_operations * iop;
	struct shall_fsinfo * fi;
	struct inode * root;
	loff_t data_end;
	int err = 0;
	char * datacopy;
	/* are they asking to mount read-only? bit pointless; note however
	 * that we do allow to remount read-only later as that could happen
	 * for example during shutdown */
	if (sb->s_flags & MS_RDONLY) {
		printk(KERN_ERR
		       "Doesn't really make sense to mount readonly\n");
		return -EINVAL;
	}
	/* see if the first superblock is valid */
	if (! sb_set_blocksize(sb, SHALL_DEV_BLOCK)) {
		printk(KERN_ERR "Unable to set blocksize\n");
		return -EINVAL;
	}
	/* allocate space to store our data */
	fi = kmalloc(sizeof(*fi), GFP_KERNEL);
	if (! fi) return -ENOMEM;
	fi->sb = sb;
	sb->s_fs_info = fi;
	/* read first superblock in fi->sbi */
	err = shall_read_superblock(fi, 0, 0);
	if (err < 0) {
		err = search_superblock(fi);
		if (err < 0) goto out_kfree_fi;
	}
	/* interrupted in the middle of an update?  Cannot mount until
	 * they complete it */
	if (fi->sbi.ro.flags & SHALL_SB_UPDATE) {
		printk(KERN_ERR "FIlesystem is in the middle of an update\n");
		err = -EAGAIN;
		goto out_kfree_fi;
	}
	/* if first superblock is dirty, find the best one */
	if (fi->sbi.ro.flags & SHALL_SB_DIRTY)
		scan_all_superblocks(fi);
	/* parse mount options */
	datacopy = kstrdup(data, GFP_KERNEL);
	if (! datacopy) {
		err = -ENOMEM;
		goto out_kfree_fi;
	}
	fi->options = default_options;
	err = parse_options(datacopy, &fi->options);
	kfree(datacopy);
	if (err) goto out_kfree_fi;
	/* check that the fs= option was provided */
	if (! fi->options.fspath) {
		printk(KERN_ERR "Missing \"fs=\" option for shallFS\n");
		err = -EINVAL;
		goto out_kfree_fi;
	}
	/* and find the underlying path following symlinks etc */
	err = kern_path(fi->options.fspath, LOOKUP_FOLLOW, &fi->root_path);
	if (err) {
		printk(KERN_ERR "Path \"%s\" not found\n", fi->options.fspath);
		goto out_kfree_fi;
	}
	if (! S_ISDIR(fi->root_path.dentry->d_inode->i_mode)) {
		printk(KERN_ERR "Path \"%s\" is not a directory\n",
		       fi->options.fspath);
		err = -ENOTDIR;
		goto out_putpath;
	}
	/* doesn't make sense to have a read-only filesystem! */
	if (fi->root_path.dentry->d_sb->s_flags & MS_RDONLY) {
		printk(KERN_ERR "Path \"%s\" is on read-only filesystem\n",
		       fi->options.fspath);
		err = -EINVAL;
		goto out_putpath;
	}
	/* check that the underlying filesystem is friendly; probably
	 * no NFS etc as that will be rather confusing; also no special
	 * permission checks (ext2/3/4 will be OK with that) */
	dop = fi->root_path.dentry->d_op;
	if (dop && (dop->d_manage ||
		    dop->d_automount ||
		    dop->d_revalidate ||
		    dop->d_weak_revalidate))
	{
		printk(KERN_ERR "Path \"%s\": unsopported filesystem type\n",
		       fi->options.fspath);
		err = -EINVAL;
		goto out_putpath;
	}
	iop = fi->root_path.dentry->d_inode->i_op;
	if (iop && iop->permission) {
		printk(KERN_ERR "Path \"%s\": unsopported filesystem type\n",
		       fi->options.fspath);
		err = -EINVAL;
		goto out_putpath;
	}
	sb->s_stack_depth = 1 + fi->root_path.dentry->d_sb->s_stack_depth;
	if (sb->s_stack_depth > FILESYSTEM_MAX_STACK_DEPTH) {
		printk(KERN_ERR "Filesystem stacked too deep\n");
		err = -EINVAL;
		goto out_putpath;
	}
	/* get a reference to this mount point so that they don't unmount
	 * it from under us */
	fi->mount = clone_private_mount(&fi->root_path);
	if (IS_ERR(fi->mount)) {
		err = PTR_ERR(fi->mount);
		goto out_putpath;
	}
	/* allocate commit buffer */
	fi->sbi.rw.other.commit_buffer = vmalloc(fi->options.commit_size);
	if (! fi->sbi.rw.other.commit_buffer) {
		err = -ENOMEM;
		goto out_putmount;
	}
	/* now go and get the root inode from the underlying filesystem,
	 * and use that to make up a root inode for us */
	root = shall_new_inode(sb, fi->root_path.dentry);
	if (IS_ERR(root)) {
		err = PTR_ERR(root);
		goto out_vfree_commit;
	}
	sb->s_root = d_make_root(root);
	if (! sb->s_root) {
		err = -ENOENT;
		goto out_vfree_commit;
	}
	sb->s_root->d_fsdata = fi->root_path.dentry;
	dget(sb->s_root->d_fsdata);
	sb->s_time_gran = 1;
	sb->s_magic = SHALL_MAGIC;
	sb->s_op = &shall_super_ops;
	sb->s_xattr = sb_xattr_handler;
	sb->s_flags |= MS_POSIXACL;
	/* create /proc entries for our userpace interface */
	err = create_proc_entries(fi);
	if (err) goto out_vfree_commit;
	/* initialise remaining bits of fi */
	sb->s_time_gran = 1;
	now = current_fs_time(sb);
	fi->sbi.ro.mounted = current_fs_time(sb);
	fi->sbi.ro.maxptr.block = fi->sbi.ro.data_space / SHALL_DEV_BLOCK;
	fi->sbi.ro.maxptr.n_super = fi->sbi.ro.num_superblocks;
	fi->sbi.ro.maxptr.offset = SHALL_DEV_BLOCK;
	fi->sbi.rw.other.last_commit = now.tv_sec;
	fi->sbi.rw.other.logged = 0;
	fi->sbi.rw.other.commit_count[0] = 0;
	fi->sbi.rw.other.commit_count[1] = 0;
	fi->sbi.rw.other.commit_count[2] = 0;
	fi->sbi.rw.read.committed = fi->sbi.rw.read.data_length;
	shall_calculate_block(fi->sbi.rw.read.data_start,
			      fi->sbi.ro.num_superblocks,
			      &fi->sbi.rw.read.startptr);
	data_end = fi->sbi.rw.read.data_start + fi->sbi.rw.read.data_length;
	if (data_end >= fi->sbi.ro.data_space)
		data_end -= fi->sbi.ro.data_space;
	shall_calculate_block(data_end, fi->sbi.ro.num_superblocks,
			      &fi->sbi.rw.read.commitptr);
	fi->sbi.rw.read.buffer_read = 0;
	fi->sbi.rw.read.buffer_written = 0;
	fi->lq.num_dropped = 0;
	fi->lq.extra_space = 0;
	mutex_init(&fi->sbi.mutex);
	init_waitqueue_head(&fi->lq.log_queue);
	init_waitqueue_head(&fi->sbi.ro.data_queue);
	atomic_set(&fi->sbi.ro.logs_reading, 0);
	atomic_set(&fi->sbi.ro.logs_writing, 0);
	atomic_set(&fi->sbi.ro.logs_valid, 1);
	atomic_set(&fi->sbi.ro.allow_commit_thread, 1);
	atomic_set(&fi->sbi.ro.inside_commit, 0);
	atomic_set(&fi->sbi.ro.some_data, fi->sbi.rw.read.data_length > 0);
	/* create (but don't yet start) a thread to do background commits */
	fi->commit_thread =
		kthread_create(shall_commit_thread, fi, "shallfs:%x:%x",
			       MAJOR(sb->s_dev), MINOR(sb->s_dev));
	if (IS_ERR(fi->commit_thread)) {
		err = PTR_ERR(fi->commit_thread);
		printk(KERN_ERR "Cannot initialise commit thread\n");
		goto out_remove_proc;
	}
	/* mark superblock dirty and update */
	fi->sbi.ro.flags |= SHALL_SB_DIRTY;
	err = shall_update_superblock(fi);
	if (err) {
		printk(KERN_ERR "Could not update superblock\n");
		goto out_stop_thread;
	}
	/* link this to our list of mounted filesystems */
	mutex_lock(&shall_fs_mutex);
	fi->next = shall_fs_list;
	fi->prev = NULL;
	shall_fs_list = fi;
	mutex_unlock(&shall_fs_mutex);
	/* log the mount operation */
	shall_log_1n(fi, SHALL_MOUNT, (char *)data, 0);
	/* log our [kv]mallocs if they said debug=on */
#ifdef CONFIG_SHALL_FS_DEBUG
	if (IS_DEBUG(fi)) {
		char buf[32];
		snprintf(buf, sizeof(buf), "kmalloc(%d)=%p",
			 (int)sizeof(*fi), fi);
		shall_log_debug(fi, buf);
		snprintf(buf, sizeof(buf), "kmalloc(?)=%p",
			 fi->options.data);
		shall_log_debug(fi, buf);
		snprintf(buf, sizeof(buf), "vmalloc(%d)=%p",
			 fi->options.commit_size,
			 fi->sbi.rw.other.commit_buffer);
		shall_log_debug(fi, buf);
	}
#endif
	/* start the commit thread */
	atomic_set(&fi->sbi.ro.thread_running, 1);
	wake_up_process(fi->commit_thread);
	return 0;
out_stop_thread:
	kthread_stop(fi->commit_thread);
out_remove_proc:
	proc_remove(fi->proc);
out_vfree_commit:
	vfree(fi->sbi.rw.other.commit_buffer);
out_putmount:
	mntput(fi->mount);
out_putpath:
	path_put(&fi->root_path);
out_kfree_fi:
	kfree(fi);
	return err < 0 ? err : -EINVAL;
}

static struct dentry *shall_mount(struct file_system_type *fs_type, int flags,
				  const char *dev_name, void *data)
{
	return mount_bdev(fs_type, flags, dev_name, data, shall_fill_super);
}

static struct file_system_type shall_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "shallfs",
	.mount		= shall_mount,
	.kill_sb	= kill_block_super,
	.fs_flags	= FS_REQUIRES_DEV,
};
MODULE_ALIAS_FS("shallfs");

static int __init shall_init_fs(void) {
	int err = 0;
	shall_fs_list = NULL;
	mutex_init(&shall_fs_mutex);
	fs_proc =  proc_mkdir("fs/shallfs", NULL);
	if (! fs_proc) {
		printk(KERN_ERR "Cannot create /proc/fs/shallfs\n");
		return -ENOENT;
	}
	proc_create("mounted", 0, fs_proc, &shall_proc_mounted);
	err = register_filesystem(&shall_fs_type);
	if (err) {
		proc_remove(fs_proc);
		return err;
	}
	return 0;
}

static void __exit shall_exit_fs(void) {
	unregister_filesystem(&shall_fs_type);
	proc_remove(fs_proc);
}

MODULE_AUTHOR("Claudio Calvelli");
MODULE_DESCRIPTION("Transparent, modification-logging, filesystem");
MODULE_LICENSE("GPL");
module_init(shall_init_fs);
module_exit(shall_exit_fs);

