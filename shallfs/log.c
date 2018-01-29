/*
 *  linux/fs/shallfs/log.c
 */

#include <linux/module.h>
#include <linux/string.h>
#include <linux/namei.h>
#include <linux/time.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/crc32.h>
#include <linux/buffer_head.h>
#include <linux/atomic.h>
#include <linux/kthread.h>
#include <linux/posix_acl.h>
#include <linux/ctype.h>
#include <linux/vmalloc.h>
#include <shallfs/operation.h>
#include <shallfs/device.h>
#include "shallfs.h"
#include "super.h"
#include "device.h"
#include "log.h"

#ifdef CONFIG_SHALL_FS_DEBUG
/* we share this file between userspace and kernel, but only if we
 * asked for kernel debug */
#include <shallfs/opdata.h>

/* structure used to hold log data when printing logs */
struct shall_header {
	unsigned int next_header;      /* offset to next header */
	enum shall_log_flags flags;    /* determine which data is present */
	struct timespec requested;     /* when the operation was requested */
	unsigned int operation;        /* operation: see below */
	int result;                    /* operation result, if known */
};
#endif

/* see add_padding */
static const unsigned char pad_zero[64] = { 0, };

/* each mounted shallfs runs a commit thread which just sleeps commit_seconds,
 * checks for data to commit, repeat; the commit is done in two phases, a
 * readahead and a commit, to minimise the time the lock is held */
int shall_commit_thread(void * _fi) {
	struct shall_fsinfo *fi = _fi;
	mutex_lock(&fi->sbi.mutex);
	while (! kthread_should_stop()) {
		struct timespec now;
		signed long timediff, timeout;
		/* figure out how long ago a commit happened, and schedule
		 * a timeout to wait for the next time a commit is due */
		now = current_kernel_time();
		timediff = now.tv_sec - fi->sbi.rw.other.last_commit;
		timeout = fi->options.commit_seconds - timediff;
		if (timeout > 0) goto wait_timeout;
		/* commit is due now; however, if we've been asked not to
		 * commit, sleep anyway */
		if (! atomic_read(&fi->sbi.ro.allow_commit_thread))
			goto wait_full;
		/* record that a commit is running... we don't expect
		 * two commit threads to run here but for maximum paranoia
		 * we actually test-and-set */
		if (atomic_xchg(&fi->sbi.ro.inside_commit, 1))
			goto wait_full;
		/* Run a commit; we unlock and run the commit without the
		 * lock so we don't delay real operations; the worst which
		 * can happen is that we find the work already done for us
		 * and I'm sure we can live with that */
		mutex_unlock(&fi->sbi.mutex);
		shall_write_data(fi, 0, 0, 1);
		/* we've done this pass */
		atomic_set(&fi->sbi.ro.inside_commit, 0);
		/* re-lock because the start of the loop expects it */
		mutex_lock(&fi->sbi.mutex);
		/* we could just fall through to wait_full, as we know
		 * we've just done a commit - but we don't know how long
		 * we've waited for the mutex; also don't know if we have
		 * been asked to stop while waiting; better to just repeat
		 * the loop */
		continue;
	wait_full:
		/* wait a full commit cycle, if we are here there's a
		 * commit running right now, so no point waiting any less */
		timeout = fi->options.commit_seconds;
	wait_timeout:
		/* unlock and sleep for the required time */
		mutex_unlock(&fi->sbi.mutex);
		schedule_timeout_killable(HZ * timeout);
		mutex_lock(&fi->sbi.mutex);
		/* somebody may have run a commit while we were
		 * sleeping, so repeat the loop to recalculate */
	}
	mutex_unlock(&fi->sbi.mutex);
	atomic_set(&fi->sbi.ro.thread_running, 0);
	return 0;
}

/* this is called by remount, umount, etc to commit all logs; they can
 * also ask to execute some code before unlocking */
void shall_commit_logs(struct shall_fsinfo *fi,
		       void (*func)(void *), void * data)
{
	int allow;
	/* first, wait for the commit thread to be idle: we know that
	 * because ro->inside_commit tells us; we also ask it not to
	 * run again until we say so */
	allow = atomic_xchg(&fi->sbi.ro.allow_commit_thread, 0);
	mutex_lock(&fi->sbi.mutex);
	while (atomic_read(&fi->sbi.ro.inside_commit)) {
		/* wait this out; we use the log_queue to wait as that'll
		 * wake us up as soon as the commit finishes... there's a
		 * chance of a commit started by the logs filling up, but
		 * it's unlikely, and if that happens, we just go through
		 * the loop again */
		mutex_unlock(&fi->sbi.mutex);
		wait_event_interruptible(fi->lq.log_queue,
			! atomic_read(&fi->sbi.ro.inside_commit));
		mutex_lock(&fi->sbi.mutex);
	}
	/* our turn, commit and run any code they asked us to run, finally
	 * unlock */
	shall_write_data(fi, 1, 2, 1);
	if (func) func(data);
	mutex_unlock(&fi->sbi.mutex);
	/* re-allow commits if we did find them allowed */
	if (allow) atomic_set(&fi->sbi.ro.allow_commit_thread, allow);
}

#ifdef CONFIG_SHALL_FS_DEBUG
/* the following code is used to provide the "hlog" file (see proc.c) */

/* helper function to add a string to another string */
static inline int add_string_len(char * __user *dest, int *len,
				 const char *what, int wlen)
{
	if (wlen < 1) return 0;
	if (wlen > *len) return -EFBIG;
	if (copy_to_user(*dest, what, wlen)) return -EFAULT;
	*dest += wlen;
	*len -= wlen;
	return 0;
}
#define add_string(d, l, w) add_string_len((d), (l), (w), strlen((w)))

/* helper function to add a name/number to a string */
static inline int add_number(char * __user *dest, int *len,
			     const char * name, int number)
{
	char nbuffer[32];
	int err;
	snprintf(nbuffer, sizeof(nbuffer), "%d", number);
	err = add_string(dest, len, name);
	if (err == 0) err = add_string(dest, len, nbuffer);
	return err;
}

/* helper function to add a name/big number to a string */
static inline int add_bignum(char * __user *dest, int *len,
			     const char * name, int64_t number)
{
	char nbuffer[32];
	int err;
	snprintf(nbuffer, sizeof(nbuffer), "%lld", (long long)number);
	err = add_string(dest, len, name);
	if (err == 0) err = add_string(dest, len, nbuffer);
	return err;
}

/* helper function to add a timestamp to a string; note that the timestamp
 * is not translated to human-readable format, use readshallfs for that */
static inline int add_time(char * __user *dest, int *len,
			   const char *name, const struct timespec *time)
{
	char nbuffer[32];
	int err;
	snprintf(nbuffer, sizeof(nbuffer), "%lld.%09lld",
		 (long long)time->tv_sec, (long long)time->tv_nsec);
	err = add_string(dest, len, name);
	if (err == 0) err = add_string(dest, len, nbuffer);
	return err;
}

/* helper function to add an octal number to a string */
static inline int add_onumber(char * __user *dest, int *len,
			      const char *name, int number)
{
	char nbuffer[32];
	int err;
	snprintf(nbuffer, sizeof(nbuffer), "%o", number);
	err = add_string(dest, len, name);
	if (err == 0) err = add_string(dest, len, nbuffer);
	return err;
}

/* helper function to add a hex number to a string */
static inline int add_xnumber(char * __user *dest, int *len,
			      const char *name, int number)
{
	char nbuffer[32];
	int err;
	snprintf(nbuffer, sizeof(nbuffer), "%x", number);
	err = add_string(dest, len, name);
	if (err == 0) err = add_string(dest, len, nbuffer);
	return err;
}

/* helper function to add a device number to a string */
static inline int add_rdev(char * __user *dest, int *len,
			   const char *name, uint64_t dev)
{
	unsigned int major = dev >> 32, minor = dev & 0xffffffff;
	int err;
	err = add_xnumber(dest, len, name, major);
	if (err == 0) err = add_xnumber(dest, len, ":", minor);
	return err;
}

/* add a single character */
static inline int add_char(char * __user *dest, int *len, char c) {
	if (*len < 1) return -EFBIG;
	if (copy_to_user(dest, &c, 1)) return -EFAULT;
	(*dest)++;
	(*len)--;
	return 0;
}

/* helper function to dump a shall_attr structure to a string */
static inline int add_attr(char * __user *dest, int *len,
			   const struct shall_devattr *da)
{
	int err = 0;
	enum shall_attr_flags flags = le32_to_cpu(da->flags);
	if ((flags & shall_attr_mode) && err == 0)
		err = add_onumber(dest, len, " mode=", le32_to_cpu(da->mode));
	if ((flags & shall_attr_user) && err == 0)
		err = add_number(dest, len, " uid=", le32_to_cpu(da->user));
	if ((flags & shall_attr_group) && err == 0)
		err = add_number(dest, len, " gid=", le32_to_cpu(da->group));
	if ((flags & shall_attr_block) && err == 0)
		err = add_rdev(dest, len, " bdev=", le64_to_cpu(da->size));
	if ((flags & shall_attr_char) && err == 0)
		err = add_rdev(dest, len, " cdev=", le64_to_cpu(da->size));
	if ((flags & shall_attr_size) && err == 0)
		err = add_bignum(dest, len, " size=", le64_to_cpu(da->size));
	if ((flags & shall_attr_atime) && err == 0) {
		struct timespec atime;
		atime.tv_sec = le64_to_cpu(da->atime_sec);
		atime.tv_nsec = le32_to_cpu(da->atime_nsec);
		err = add_time(dest, len, " atime=", &atime);
	}
	if ((flags & shall_attr_mtime) && err == 0) {
		struct timespec mtime;
		mtime.tv_sec = le64_to_cpu(da->mtime_sec);
		mtime.tv_nsec = le32_to_cpu(da->mtime_nsec);
		err = add_time(dest, len, " mtime=", &mtime);
	}
	return err;
}

/* helper function to print an ACL entry */
static inline int add_perms(char * __user *dest, int *len, char sep,
			    char who, int id, int perm)
{
	char ws[32];
	int wp = 0;
	ws[wp++] = sep;
	ws[wp++] = who;
	ws[wp++] = ':';
	if (id >= 0) {
		snprintf(ws + wp, sizeof(ws) - wp - 8, "%d", id);
		wp += strlen(ws + wp);
	}
	ws[wp++] = ':';
	ws[wp++] = (perm & shall_acl_read) ? 'r' : '-';
	ws[wp++] = (perm & shall_acl_write) ? 'w' : '-';
	ws[wp++] = (perm & shall_acl_execute) ? 'x' : '-';
	if (perm & shall_acl_add) ws[wp++] = 'a';
	if (perm & shall_acl_delete) ws[wp++] = 'd';
	return add_string_len(dest, len, ws, wp);
}

/* helper function to decode and print an ACL */
static int add_acl(char * __user *dest, int *len,
		   const struct shall_devacl *dl)
{
	int count = le32_to_cpu(dl->count), n = le32_to_cpu(dl->perm), err = 0;
	if (err == 0)
		err = add_string(dest, len,
			         (n & (1 << 28)) ? " access_acl"
						 : " default_acl");
	if (err == 0)
		err = add_perms(dest, len, '=', 'u', -1, n);
	if (err == 0)
		err = add_perms(dest, len, ',', 'g', -1, n >> 7);
	if (err == 0)
		err = add_perms(dest, len, ',', 'o', -1, n >> 14);
	if (err == 0)
		err = add_perms(dest, len, ',', 'm', -1, n >> 21);
	for (n = 0; n < count; n++) {
		int type = le32_to_cpu(dl->entries[n].type);
		if (err == 0)
			err = add_string(dest, len, ",");
		if (err == 0)
			err = add_perms(dest, len, ',',
					type & (1 << 28) ? 'g' : 'u',
					le32_to_cpu(dl->entries[n].name),
					type);
	}
	return err;
}

/* print data */
static int add_data(char * __user *dest, int *len, const char *name,
		    const void * dptr, int dlen)
{
	char buffer[3];
	const unsigned char * data = dptr;
	int err = add_string(dest, len, name), i;
	for (i = 0; i < dlen && err == 0; i++) {
		snprintf(buffer, sizeof(buffer), "%02x", data[i]);
		err = add_string(dest, len, buffer);
	}
	return err;
}

/* print hash */
static int add_hash(char * __user *dest, int *len, const char *name,
		    const unsigned char * hash)
{
	return add_data(dest, len, name, hash, SHALL_HASH_LENGTH);
}

/* convert information in log to string */
static int print_log(char __user *dest, int remain, int operation, int result,
		     enum shall_log_flags flags, const struct timespec *time,
		     const void *data_ptr)
{
	const struct shall_devregion *dr;
	const struct shall_devfileid *dih;
	const struct shall_devsize *dsh;
	const struct shall_devxattr *dx;
	const struct shall_devhash *dh;
	int prnop = operation, dataflag, xf, n, ne, err = 0, len = remain;
	add_time(&dest, &len, "@", time);
	if (prnop == 0) {
		if (err == 0)
			err = add_string(&dest, &len, " DEBUG");
	} else {
		if (prnop < 0) {
			prnop = -prnop;
			if (err == 0)
				err = add_string(&dest, &len, " before ");
		} else {
			if (err == 0)
				err = add_string(&dest, &len, " after ");
		}
		if (prnop < SHALL_MAX_OPCODE && shall_opdata[prnop].name) {
			if (err == 0)
				err = add_string(&dest, &len,
						 shall_opdata[prnop].name);
		} else {
			if (err == 0)
				err = add_number(&dest, &len, "op", prnop);
		}
		if (operation >= 0 && err == 0)
			err = add_number(&dest, &len, " result=", result);
	}
	dataflag = flags & SHALL_LOG_DMASK;
	switch (dataflag) {
		case SHALL_LOG_ATTR :
			if (err == 0)
				err = add_attr(&dest, &len, data_ptr);
			break;
		case SHALL_LOG_REGION :
			dr = data_ptr;
			if (err == 0)
				err = add_number(&dest, &len, " id=",
						 le32_to_cpu(dr->fileid));
			if (err == 0)
				err = add_bignum(&dest, &len, " start=",
						 le64_to_cpu(dr->start));
			if (err == 0)
				err = add_bignum(&dest, &len, " length=",
						 le64_to_cpu(dr->length));
			break;
		case SHALL_LOG_FILEID :
			dih = data_ptr;
			if (err == 0)
				err = add_number(&dest, &len, " id=",
						 le32_to_cpu(dih->fileid));
			break;
		case SHALL_LOG_SIZE :
			dsh = data_ptr;
			if (err == 0)
				err = add_bignum(&dest, &len, " size=",
						 le64_to_cpu(dsh->size));
			break;
		case SHALL_LOG_ACL :
			if (err == 0)
				err = add_acl(&dest, &len, data_ptr);
			break;
		case SHALL_LOG_XATTR :
			dx = data_ptr;
			if (err == 0)
				err = add_string(&dest, &len, " xattr[");
			ne = le32_to_cpu(dx->namelen);
			if (err == 0)
				err = add_string_len(&dest, &len, dx->data, ne);
			xf = le32_to_cpu(dx->flags);
			if (err == 0)
				err = add_xnumber(&dest, &len, ",", xf);
			xf = le32_to_cpu(dx->valuelen);
			if (err == 0)
				err = add_number(&dest, &len, "=", xf);
			if (err == 0)
				err = add_string(&dest, &len, "[");
			for (n = 0; n < xf && err == 0; n++) {
				unsigned char c = dx->data[n + ne];
				if (isascii(c) && isgraph(c) && c != '%')
					err = add_char(&dest, &len, c);
				else
					err = add_xnumber(&dest, &len, "%", c);
			}
			if (err == 0)
				err = add_string(&dest, &len, "]");
			break;
		case SHALL_LOG_HASH :
			dh = data_ptr;
			if (err == 0)
				err = add_number(&dest, &len, " id=",
						 le32_to_cpu(dh->fileid));
			if (err == 0)
				err = add_bignum(&dest, &len, " start=",
						 le64_to_cpu(dh->start));
			if (err == 0)
				err = add_bignum(&dest, &len, " length=",
						 le64_to_cpu(dh->length));
			if (err == 0)
				err = add_hash(&dest, &len, " hash=", dh->hash);
			break;
		case SHALL_LOG_DATA :
			dr = data_ptr;
			if (err == 0)
				err = add_number(&dest, &len, " id=",
						 le32_to_cpu(dr->fileid));
			if (err == 0)
				err = add_bignum(&dest, &len, " start=",
						 le64_to_cpu(dr->start));
			if (err == 0)
				err = add_bignum(&dest, &len, " length=",
						 le64_to_cpu(dr->length));
			if (err == 0)
				err = add_data(&dest, &len, " data=",
					       &dr[1], le64_to_cpu(dr->length));
			break;
	}
	return err ? err : (remain - len);
}
#endif

/* add some blob to the commit buffer; caller must hold the mutex locked */
static void add_blob(struct shall_fsinfo *fi, const void *data, int len) {
	/* this gets called after checking that there is space, and the
	 * lock has not been released after that check... so there is space
	 * and we do not look again */
	if (len > 0) {
		if (fi->sbi.rw.read.buffer_written + len >
			fi->options.commit_size)
		{
			/* yikes, how did that happen? */
			BUG();
		}
		memcpy(fi->sbi.rw.other.commit_buffer +
			fi->sbi.rw.read.buffer_written, data, len);
		fi->sbi.rw.read.buffer_written += len;
		fi->sbi.rw.read.data_length += len;
	}
}

/* add some padding to the commit buffer; caller must hold the mutex locked */
static void add_padding(struct shall_fsinfo *fi, int len) {
	/* we don't expect to ever pad more than 64 bytes; but if we
	 * do get asked to pad more, we just loop more than once */
	while (len > 0) {
		int td = len;
		if (td > sizeof(pad_zero)) td = sizeof(pad_zero);
		add_blob(fi, pad_zero, td);
		len -= td;
	}
}

/* calculate checksum on a log header: all data is stored to device in
 * little-endian format so we use crc32_le */
#define checksum_header(sh) \
	crc32_le(0x4c414853, (void *)&(sh), shall_devheader_checksize)

/* calculate required log size, given original size and alignment; we
 * can use roundup() from <linux/kernel.h> */
#define logsize(fi, l) roundup((l), (fi)->sbi.ro.log_alignment)

/* make absolutely sure the commit buffer has space for "len" bytes;
 * must be called with the mutex locked */
static inline void need_commit(struct shall_fsinfo *fi, unsigned int len) {
	if (len + fi->sbi.rw.read.buffer_written > fi->options.commit_size)
		shall_write_data(fi, 1, 1, 0);
}

/* log that the buffer wasn't big enough, and adjust stuff so we'll know
 * how big it must have been; this must be called with the mutex locked */
static void log_overflow(struct shall_fsinfo *fi, int space) {
	struct shall_devheader ovh;
	struct timespec overflowed;
	unsigned int next_header;
	int num_dropped;
	/* we need to lock the log queue; we make sure the order is mutex
	 * first, spin next, to avoid the possibility of deadlock */
	spin_lock(&fi->lq.log_queue.lock);
	num_dropped = fi->lq.num_dropped;
	fi->lq.num_dropped++;
	fi->lq.extra_space += space;
	spin_unlock(&fi->lq.log_queue.lock);
	/* if we found num_dropped > 0, somebody else has already added the
	 * overflow event, and we've just updated the other data, so nothing
	 * left to do */
	if (num_dropped > 0) return;
	/* this has not yet been reported, so report it; we always keep
	 * enough space for this event, as long as we only report it once;
	 * however out of paranoia we go and double check */
	next_header = logsize(fi, sizeof(ovh));
	if (next_header + fi->sbi.rw.read.data_length > fi->sbi.ro.data_space) {
		printk(KERN_ERR
		       "Internal error in shallfs: "
		       "did not keep space for overflow log\n");
		return;
	}
	overflowed = current_kernel_time();
	need_commit(fi, next_header);
	ovh.next_header = cpu_to_le32(next_header);
	ovh.operation = cpu_to_le32(SHALL_OVERFLOW);
	ovh.req_sec = cpu_to_le64(overflowed.tv_sec);
	ovh.req_nsec = cpu_to_le32(overflowed.tv_nsec);
	ovh.result = cpu_to_le32(0);
	ovh.flags = cpu_to_le32(SHALL_LOG_NODATA);
	ovh.checksum = cpu_to_le32(checksum_header(ovh));
	add_blob(fi, &ovh, sizeof(ovh));
	if (next_header > sizeof(ovh))
		add_padding(fi, next_header - sizeof(ovh));
	if (fi->sbi.rw.read.buffer_written >= fi->options.commit_size)
		shall_write_data(fi, 1, 1, 0);
	fi->sbi.rw.other.logged++;
	if (fi->sbi.rw.other.max_length < fi->sbi.rw.read.data_length)
		fi->sbi.rw.other.max_length = fi->sbi.rw.read.data_length;
	atomic_set(&fi->sbi.ro.some_data, 1);
	wake_up_all(&fi->sbi.ro.data_queue);
}

/* add a new log to the device and/or the memory cache; caller must not
 * hold the mutex already locked */
static int append_logs(struct shall_fsinfo *fi, int operation, int result,
		       enum shall_log_flags flags,
		       const void *dptr[], int dlen[])
{
	struct shall_devheader lh;
	struct shall_devfileid dih;
	struct timespec requested = current_kernel_time();
	unsigned int next_header, required, padding;
	int err, data, dataflag;
retry_logging:
	next_header = sizeof(lh);
	err = data = 0;
	if (flags & SHALL_LOG_FILE1) {
		next_header += sizeof(dih) + dlen[0];
		data++;
	}
	if (flags & SHALL_LOG_FILE2) {
		next_header += sizeof(dih) + dlen[1];
		data++;
	}
	dataflag = flags & SHALL_LOG_DMASK;
	if (dataflag) next_header += dlen[data];
	padding = next_header;
	next_header = logsize(fi, next_header);
	padding = next_header - padding;
	lh.next_header = cpu_to_le32(next_header);
	lh.operation = cpu_to_le32(operation);
	lh.req_sec = cpu_to_le64(requested.tv_sec);
	lh.req_nsec = cpu_to_le32(requested.tv_nsec);
	lh.result = cpu_to_le32(result);
	lh.flags = cpu_to_le32(flags);
	lh.checksum = cpu_to_le32(checksum_header(lh));
retry_size_check:
	if (next_header > fi->options.commit_size) {
		/* log will never fit! */
		if (operation == SHALL_TOO_BIG)
			/* yikes, a microscopic buffer */
			return -EFBIG;
		printk(KERN_ERR
		       "shallfs(%s): log does not fit in memory buffer, "
		       "available %d, required %u\n",
		       fi->options.fspath,
		       fi->options.commit_size, next_header);
		if (IS_ERROR(fi))
			return -EFBIG;
		operation = SHALL_TOO_BIG;
		result = next_header;
		flags = SHALL_LOG_NODATA;
		goto retry_logging;
	}
	/* calculate space required to store the log, while keeping enough
	 * space to store an overflow log */
	required = logsize(fi, sizeof(lh)) + next_header;
	/* OK, ready to store, get that mutex locked */
	mutex_lock(&fi->sbi.mutex);
	/* somebody might have started a remount while we were waiting for
	 * the lock, and the remount's commit may still be running; better
	 * unlock and wait it out!   We can use the log_queue to wait as
	 * we know they are going to wake us up there; note that an umount
	 * would also clear allow_commit_thread and never set it again...
	 * but that's OK as the umount will not start while we are in the
	 * middle of an operation */
	while (unlikely(! atomic_read(&fi->sbi.ro.allow_commit_thread))) {
		mutex_unlock(&fi->sbi.mutex);
		err = wait_event_interruptible(fi->lq.log_queue,
				atomic_read(&fi->sbi.ro.allow_commit_thread));
		if (err) return err;
		mutex_lock(&fi->sbi.mutex);
	}
	/* now this may sound silly, but what if somebody remounted with
	 * a smaller commit size while we were waiting to get the mutex?
	 * oh well, better have another look, but if this IS the "too big"
	 * log and they managed to make the buffer so small it doesn't fit,
	 * we don't bother re-reporting it as that would just loop */
	if (unlikely(next_header > fi->options.commit_size)) {
		mutex_unlock(&fi->sbi.mutex);
		if (lh.operation == cpu_to_le32(SHALL_TOO_BIG))
			return 0;
		goto retry_size_check;
	}
	/* the event will fit in the memory buffer (for now... see other
	 * comments) but will it fit in the device? */
	if (required + fi->sbi.rw.read.data_length > fi->sbi.ro.data_space) {
		/* not enough space, log an overflow event and then either
		 * drop this event or wait for space, depending on the
		 * mount options */
		log_overflow(fi, next_header);
		/* if they said overflow=drop, do so */
		if (IS_DROP(fi)) goto out_noerror;
		/* we now wait for somebody to make space; we wait in a
		 * loop because we don't know if the space will be enough,
		 * just that there will be some space */
		while (required + fi->sbi.rw.read.data_length >
		       fi->sbi.ro.data_space)
		{
			/* now we wait for one of three things:
			 * 1. somebody makes space: we repeat the loop to
			 *    see if it is enough
			 * 2. we receive a signal: that means drop the log
			 * 3. they remount with overflow=drop, which also
			 *    means that we must drop the log */
			mutex_unlock(&fi->sbi.mutex);
			/* now we need to acquire the queue lock and sleep */
			spin_lock(&fi->lq.log_queue.lock);
			err = wait_event_interruptible_locked(fi->lq.log_queue,
				! fi->lq.num_dropped || IS_DROP(fi));
			spin_unlock(&fi->lq.log_queue.lock);
			/* error means we received a signal: let the
			 * userspace do something about that */
			if (err) return err;
			/* re-check the mount options, if we are now dropping
			 * these events, there's nothing more to do */
			if (IS_DROP(fi)) return 0;
			/* there's space, but will that be enough?  We
			 * acquire the mutex so we can check (and somebody
			 * may steal the space from us if we sleep here,
			 * which will look to us as "not enough space", but
			 * FIXME would be good to have a mechanism to enforce
			 * ordering here so that only operations requested
			 * before us can steal the space) */
			mutex_lock(&fi->sbi.mutex);
			/* well, believe this or not, but somebody may have
			 * done a remount while we waited for the mutex;
			 * this check probably qualifies as "double
			 * paranoia" but we do it anyway */
			if (IS_DROP(fi)) goto out_noerror;
			/* or they may have shrunk the commit buffer on us;
			 * not the best idea... but we need to check again;
			 * and like before, we also need to check if the
			 * operation was already a "too big"... */
			if (next_header > fi->options.commit_size) {
				mutex_unlock(&fi->sbi.mutex);
				if (lh.operation == cpu_to_le32(SHALL_TOO_BIG))
					return 0;
				goto retry_size_check;
			}
		}
	}
	/* OK, we have enough space in the buffer, we have enough space in
	 * the device, and we have the mutex, time to store all that data */
	need_commit(fi, next_header);
	add_blob(fi, &lh, sizeof(lh));
	if (flags & SHALL_LOG_FILE1) {
		dih.fileid = cpu_to_le32(dlen[0]);
		add_blob(fi, &dih, sizeof(dih));
		add_blob(fi, dptr[0], dlen[0]);
	}
	if (flags & SHALL_LOG_FILE2) {
		dih.fileid = cpu_to_le32(dlen[1]);
		add_blob(fi, &dih, sizeof(dih));
		add_blob(fi, dptr[1], dlen[1]);
	}
	if (dataflag) add_blob(fi, dptr[data], dlen[data]);
	if (padding > 0) add_padding(fi, padding);
out_noerror:
	if (fi->sbi.rw.other.max_length < fi->sbi.rw.read.data_length)
		fi->sbi.rw.other.max_length = fi->sbi.rw.read.data_length;
	mutex_unlock(&fi->sbi.mutex);
	atomic_set(&fi->sbi.ro.some_data, 1);
	wake_up_all(&fi->sbi.ro.data_queue);
	return 0;
}

/* log an event with 0 filenames and no other data */
int shall_log_0n(struct shall_fsinfo *fi, int operation, int result) {
	return append_logs(fi, operation, result, SHALL_LOG_NODATA, NULL, NULL);
}

/* log an event with 0 filenames and integer data (fileid) */
int shall_log_0i(struct shall_fsinfo *fi, int operation,
		 int fileid, int result)
{
	struct shall_devfileid dfi;
	const void * ptr = &dfi;
	int len = sizeof(dfi);
	dfi.fileid = cpu_to_le32(fileid);
	return append_logs(fi, operation, result, SHALL_LOG_FILEID, &ptr, &len);
}

/* log an event with 0 filenames and a region structure */
int shall_log_0r(struct shall_fsinfo *fi, int operation,
		 loff_t start, size_t length, int fileid, int result)
{
	struct shall_devregion dr;
	const void * ptr = &dr;
	int len = sizeof(dr);
	dr.start = cpu_to_le64(start);
	dr.length = cpu_to_le64(length);
	dr.fileid = cpu_to_le32(fileid);
	return append_logs(fi, operation, result, SHALL_LOG_REGION, &ptr, &len);
}

/* log an event with 0 filenames and hash of data changed */
int shall_log_0h(struct shall_fsinfo *fi, int operation,
		 loff_t start, size_t length, const char __user *data,
		 int fileid, int result)
{
	struct shall_devhash dh;
	const void * ptr = &dh;
	int len = sizeof(dh);
	dh.start = cpu_to_le64(start);
	dh.length = cpu_to_le64(length);
	dh.fileid = cpu_to_le32(fileid);
	return -ENOSYS; // XXX calculate hash to dh.hash
	return append_logs(fi, operation, result, SHALL_LOG_HASH, &ptr, &len);
}

/* log an event with 0 filenames and copy of data changed */
int shall_log_0d(struct shall_fsinfo *fi, int operation,
		 loff_t start, size_t length, const char __user *data,
		 int fileid, int result)
{
	/* we need to copy from user before we can log... and to avoid
	 * having a large buffer we split the operation into chunks */
	char buffer[1024];
	struct shall_devregion dr;
	const void * ptr[2] = { &dr, buffer };
	int len[2] = { sizeof(dr), 0 };
	while (1) {
		size_t todo = length;
		int err;
		if (todo > sizeof(buffer)) todo = sizeof(buffer);
		len[1] = todo;
		if (copy_from_user(buffer, data, todo))
			return -EFAULT;
		data += todo;
		err = append_logs(fi, operation, result,
				  SHALL_LOG_DATA, ptr, len);
		if (err) return err;
		if (length < 1) return 0;
	}
}

/* log an event with 1 filename and no other data */
int shall_log_1n(struct shall_fsinfo *fi, int operation,
		 const char *name, int result)
{
	const void * ptr = name;
	int len = strlen(name);
	return append_logs(fi, operation, result, SHALL_LOG_FILE1, &ptr, &len);
}

/* log an event with 1 filename and integer data (fileid) */
int shall_log_1i(struct shall_fsinfo *fi, int operation,
		 const char *name, int fileid, int result)
{
	struct shall_devfileid dfi;
	const void * ptr[2] = { name, &dfi };
	int len[2] = { strlen(name), sizeof(dfi) };
	dfi.fileid = cpu_to_le32(fileid);
	return append_logs(fi, operation, result,
			   SHALL_LOG_FILE1 | SHALL_LOG_FILEID, ptr, len);
}

static inline void mkattr(struct shall_devattr * da,
			  const struct shall_attr *attr)
{
	memset(da, 0, sizeof(*da));
	da->flags = cpu_to_le32(attr->flags);
	da->mode = cpu_to_le32(attr->mode & S_IALLUGO);
	da->user = cpu_to_le32(attr->user);
	da->group = cpu_to_le32(attr->group);
	if (attr->flags & (shall_attr_block | shall_attr_char)) {
		uint64_t n = (uint64_t)MAJOR(attr->device) << 32
			   | (uint64_t)MINOR(attr->device);
		da->size = cpu_to_le64(n);
	} else {
		da->size = cpu_to_le64(attr->size);
	}
	da->atime_sec = cpu_to_le64(attr->atime.tv_sec);
	da->atime_nsec = cpu_to_le32(attr->atime.tv_nsec);
	da->mtime_sec = cpu_to_le64(attr->mtime.tv_sec);
	da->mtime_nsec = cpu_to_le32(attr->mtime.tv_nsec);
}

/* log an event with 1 filename and an "attr" structure */
int shall_log_1a(struct shall_fsinfo *fi, int operation,
		 const char *name, const struct shall_attr *attr, int result)
{
	struct shall_devattr da;
	const void * ptr[2] = { name, &da };
	int len[2] = { strlen(name), sizeof(da) };
	mkattr(&da, attr);
	return append_logs(fi, operation, result,
			   SHALL_LOG_FILE1 | SHALL_LOG_ATTR, ptr, len);
}

static inline int count_entries(const struct posix_acl *acl) {
	int n, count = 0;
	for (n = 0; n < acl->a_count; n++) {
		switch (acl->a_entries[n].e_tag) {
			case ACL_USER :
			case ACL_GROUP :
				count++;
				break;
			case ACL_USER_OBJ :
			case ACL_GROUP_OBJ :
			case ACL_OTHER :
			case ACL_MASK :
				break;
		}
	}
	return count;
}

/* log an event with 1 filename and a POSIX acl */
int shall_log_1l(struct shall_fsinfo *fi, int operation, const char *name,
		 int access, const struct posix_acl *acl, int result)
{
	int count = count_entries(acl), perm = access ? (1 << 28) : 0;
	int size = sizeof(struct shall_devacl)
		 + count * sizeof(struct shall_devacl_entry);
	struct shall_devacl * da = shall_kmalloc(fi, size, GFP_KERNEL);
	const void * ptr[2] = { name, da };
	int err, n, len[2] = { strlen(name), size }, d;
	if (! da) return -ENOMEM;
	da->count = cpu_to_le32(count);
	for (n = d = 0; n < acl->a_count; n++) {
		const struct posix_acl_entry * pe = &acl->a_entries[n];
		int name = 0, type = 0, shift = -1;
		switch (pe->e_tag) {
			case ACL_USER :
				name = from_kuid(&init_user_ns, pe->e_uid);
				break;
			case ACL_USER_OBJ :
				shift = 0;
				break;
			case ACL_GROUP :
				name = from_kgid(&init_user_ns, pe->e_gid);
				type = 1 << 28;
				break;
			case ACL_GROUP_OBJ :
				shift = 7;
				break;
			case ACL_OTHER :
				shift = 14;
				break;
			case ACL_MASK :
				shift = 21;
				break;
		}
		if (pe->e_perm & ACL_READ)
			type |= shall_acl_read;
		if (pe->e_perm & ACL_WRITE)
			type |= shall_acl_write;
		if (pe->e_perm & ACL_EXECUTE)
			type |= shall_acl_execute;
		if (shift < 0) {
			struct shall_devacl_entry * de = &da->entries[d];
			d++;
			de->type = cpu_to_le32(type);
			de->name = cpu_to_le32(name);
		} else {
			perm |= (type & 0x7f) << shift;
		}
	}
	da->perm = cpu_to_le32(perm);
	err = append_logs(fi, operation, result,
			  SHALL_LOG_FILE1 | SHALL_LOG_ACL, ptr, len);
	shall_kfree(fi, da);
	return err;
}

/* log an event with 1 filename and a POSIX extended attribute */
int shall_log_1x(struct shall_fsinfo *fi, int operation, const char *file,
		 const char *attr, const void *value, size_t size,
		 int flags, int result)
{
	int attrlen = strlen(attr), err;
	int totlen = attrlen + size + sizeof(struct shall_devxattr);
	int len[2] = { strlen(file), totlen };
	struct shall_devxattr * dp = shall_kmalloc(fi, totlen, GFP_KERNEL);
	const void * ptr[2] = { file, dp };
	if (! dp) return -ENOMEM;
	dp->flags = cpu_to_le32(flags);
	dp->namelen = cpu_to_le32(attrlen);
	dp->valuelen = cpu_to_le32(size);
	strncpy(dp->data, attr, attrlen);
	if (size > 0)
		memcpy(dp->data + attrlen, value, size);
	err = append_logs(fi, operation, result,
			  SHALL_LOG_FILE1 | SHALL_LOG_XATTR, ptr, len);
	shall_kfree(fi, dp);
	return err;
}

/* log an event with 2 filenames and no other data */
int shall_log_2n(struct shall_fsinfo *fi, int operation,
		 const char *name1, const char *name2, int result)
{
	const void * ptr[2] = { name1, name2 };
	int len[2] = { strlen(name1), strlen(name2) };
	return append_logs(fi, operation, result,
			   SHALL_LOG_FILE1 | SHALL_LOG_FILE2, ptr, len);
}

/* log an event with 2 filenames and an "attr" structure */
int shall_log_2a(struct shall_fsinfo *fi, int operation,
		 const char *name1, const char *name2,
		 const struct shall_attr *attr, int result)
{
	struct shall_devattr da;
	const void * ptr[3] = { name1, name2, &da };
	int len[3] = { strlen(name1), strlen(name2), sizeof(da) };
	mkattr(&da, attr);
	return append_logs(fi, operation, result,
			   SHALL_LOG_FILE1 | SHALL_LOG_FILE2 | SHALL_LOG_ATTR,
			   ptr, len);
}

/* log recovery from overflow; caller must hold the mutex */
static void shall_log_recovery(struct shall_fsinfo *fi) {
	struct shall_devheader sh;
	struct shall_devsize dsh;
	struct timespec recovered;
	int64_t extra_space;
	int data_size = sizeof(sh) + sizeof(dsh), num_dropped;
	int next_header = logsize(fi, data_size);
	int required = next_header + logsize(fi, sizeof(sh));
	if (required + fi->sbi.rw.read.data_length > fi->sbi.ro.data_space)
		return
	/* lock the queue... note order of locking to avoid deadlock, the
	 * mutex first (done by caller), the spin lock next */
	spin_lock(&fi->lq.log_queue.lock);
	if (fi->lq.num_dropped == 0) {
		spin_unlock(&fi->lq.log_queue.lock);
		return;
	}
	extra_space = fi->lq.extra_space;
	num_dropped = fi->lq.num_dropped;
	fi->lq.extra_space = 0;
	fi->lq.num_dropped = 0;
	spin_unlock(&fi->lq.log_queue.lock);
	recovered = current_kernel_time();
	need_commit(fi, next_header);
	dsh.size = cpu_to_le64(extra_space);
	sh.next_header = cpu_to_le32(next_header);
	sh.operation = cpu_to_le32(SHALL_RECOVER);
	sh.req_sec = cpu_to_le64(recovered.tv_sec);
	sh.req_nsec = cpu_to_le32(recovered.tv_nsec);
	sh.result = cpu_to_le32(num_dropped);
	sh.flags = cpu_to_le32(SHALL_LOG_SIZE);
	sh.checksum = cpu_to_le32(checksum_header(sh));
	add_blob(fi, &sh, sizeof(sh));
	add_blob(fi, &dsh, sizeof(dsh));
	if (next_header > data_size)
		add_padding(fi, next_header - data_size);
	if (fi->sbi.rw.read.buffer_written >= fi->options.commit_size)
		shall_write_data(fi, 1, 1, 0);
	fi->sbi.rw.other.logged++;
	if (fi->sbi.rw.other.max_length < fi->sbi.rw.read.data_length)
		fi->sbi.rw.other.max_length = fi->sbi.rw.read.data_length;
	atomic_set(&fi->sbi.ro.some_data, 1);
	wake_up_all(&fi->sbi.ro.data_queue);
}

/* read next log header; called with mutex locked; returns the total length
 * of the event, 0 if not enough data available, or negative if error */
static int get_log_devheader(struct shall_fsinfo *fi,
			     struct shall_devheader *evh)
{
	int next_header, err, chk;
	err = shall_read_data_kernel(fi, evh, sizeof(*evh));
	if (err <= 0) return err;
	chk = checksum_header(*evh);
	if (chk != le32_to_cpu(evh->checksum)) return -EINVAL;
	next_header = le32_to_cpu(evh->next_header);
	if (next_header < sizeof(*evh)) return -EINVAL;
	if (fi->sbi.rw.read.data_length < next_header - sizeof(*evh))
		return -EINVAL;
	return next_header;
}

/* retrieves logs from device and/or memory buffer and store it in the
 * memory area provided; returns the amount of buffer actually used,
 * which may be 0 if there was nothing available, or negative if an
 * error occurred */
ssize_t shall_bin_logs(struct shall_fsinfo *fi,
		       char __user *buffer, size_t space)
{
	struct shall_sbinfo_rw_read save;
	struct shall_devheader evh;
	ssize_t done = 0, err = 0;
	if (space < 1) return 0;
	mutex_lock(&fi->sbi.mutex);
	save = fi->sbi.rw.read;
	while (space >= sizeof(evh)) {
		/* read next event header and make sure it's valid */
		int next_header, err;
		save = fi->sbi.rw.read;
		err = get_log_devheader(fi, &evh);
		if (err <= 0) goto out_restore;
		next_header = err;
		/* see if the user has enough space */
		if (space < next_header) goto out_nospace;
		/* copy header to userspace */
		if (copy_to_user(buffer, &evh, sizeof(evh))) goto out_fault;
		/* read remaining log data into userspace */
		if (next_header > sizeof(evh)) {
			err = shall_read_data_user(fi, buffer + sizeof(evh),
						   next_header - sizeof(evh));
			if (err < 0) goto out_restore;
			if (err == 0) goto out_invalid;
		}
		space -= next_header;
		buffer += next_header;
		done += next_header;
	}
	atomic_set(&fi->sbi.ro.some_data,
		   fi->sbi.rw.read.data_length >=
		   	sizeof(struct shall_devheader));
	/* if we are in an overflow situation, try to log a recovery event */
	shall_log_recovery(fi);
	/* if anybody was waiting for space... let them try */
	wake_up_all(&fi->lq.log_queue);
	mutex_unlock(&fi->sbi.mutex);
	return done;
out_invalid:
	err = -EINVAL;
	goto out_restore;
out_nospace:
	err = -EFBIG;
	goto out_restore;
out_fault:
	err = -EFAULT;
out_restore:
	fi->sbi.rw.read = save;
	atomic_set(&fi->sbi.ro.some_data,
		   fi->sbi.rw.read.data_length >=
		   	sizeof(struct shall_devheader));
	if (done > 0) {
		/* if we are in an overflow situation, try to log a
		 * recovery event */
		shall_log_recovery(fi);
		/* if anybody was waiting for space... let them try */
		wake_up_all(&fi->lq.log_queue);
	}
	mutex_unlock(&fi->sbi.mutex);
	return done > 0 ? done : err;
}

/* remove logs from journal without storing them anywhere; caller must
 * not already hold the mutex */
int shall_delete_logs(struct shall_fsinfo *fi, size_t skip) {
	struct shall_sbinfo_rw_read save;
	struct shall_devheader evh;
	ssize_t done = 0, err = 0;
	if (skip < 1) return 0;
	mutex_lock(&fi->sbi.mutex);
	save = fi->sbi.rw.read;
	while (skip >= sizeof(struct shall_devheader)) {
		/* read next event header and skip the whole thing */
		int evlen;
		save = fi->sbi.rw.read;
		err = get_log_devheader(fi, &evh);
		if (err <= 0) goto out_restore;
		evlen = err;
		if (skip < evlen) goto out_restore;
		if (evlen > sizeof(evh)) {
			int diff = evlen - sizeof(evh);
			err = shall_mark_read(fi, diff);
			if (err <= 0) goto out_restore;
		}
		skip -= evlen;
		done += evlen;
	}
	atomic_set(&fi->sbi.ro.some_data,
		   fi->sbi.rw.read.data_length >=
		   	sizeof(struct shall_devheader));
	/* if we are in an overflow situation, try to log a recovery event */
	shall_log_recovery(fi);
	/* if anybody was waiting for space... let them try */
	wake_up_all(&fi->lq.log_queue);
	mutex_unlock(&fi->sbi.mutex);
	return done;
out_restore:
	fi->sbi.rw.read = save;
	atomic_set(&fi->sbi.ro.some_data,
		   fi->sbi.rw.read.data_length >=
		   	sizeof(struct shall_devheader));
	if (done > 0) {
		/* if we are in an overflow situation, try to log a
		 * recovery event */
		shall_log_recovery(fi);
		/* if anybody was waiting for space... let them try */
		wake_up_all(&fi->lq.log_queue);
	}
	mutex_unlock(&fi->sbi.mutex);
	return done > 0 ? done : err;
}

#ifdef CONFIG_SHALL_FS_DEBUG
/* read and decode next log header; called with mutex locked; returns the
 * amount of data read, 0 if not enough data available, or negative if error */
static int get_log_header(struct shall_fsinfo *fi, struct shall_header *sh) {
	struct shall_devheader evh;
	int next_header, err;
	err = get_log_devheader(fi, &evh);
	if (err <= 0) return err;
	next_header = le32_to_cpu(evh.next_header);
	sh->next_header = next_header;
	sh->flags = le32_to_cpu(evh.flags);
	sh->requested.tv_sec = le64_to_cpu(evh.req_sec);
	sh->requested.tv_nsec = le32_to_cpu(evh.req_nsec);
	sh->operation = le32_to_cpu(evh.operation);
	sh->result = le32_to_cpu(evh.result);
	return sizeof(evh);
}

/* helper function to add a string to another string, in __user space */
static inline int add_string_user(char __user *dest, int *used,
				  const char *what)
{
	int nl = strlen(what);
	if (copy_to_user(dest + *used, what, nl)) return 0;
	*used += nl;
	return 1;
}

#define add_userstring(what) { \
	if (! add_string_user(buffer, &used, what)) \
		goto out_fault; \
}
#define read_structure(s) \
	shall_read_data_kernel(fi, &(s), sizeof((s)))

/* similar to shall_bin_logs, but produces a printable version */
ssize_t shall_print_logs(struct shall_fsinfo *fi,
			 char __user * buffer, size_t space)
{
	struct shall_sbinfo_rw_read save, svfile1, svfile2, svtemp;
	struct shall_header sh;
	struct shall_devregion drh;
	struct shall_devattr dah;
	struct shall_devfileid idh;
	struct shall_devsize dsh;
	struct shall_devacl dlh;
	struct shall_devxattr dxh;
	struct shall_devhash dhh;
	void * freeit = NULL;
	ssize_t done = 0, err = 0;
	if (space < 1) return 0;
	mutex_lock(&fi->sbi.mutex);
	save = fi->sbi.rw.read;
	while (space > 0) {
		/* read next event header and make sure it's valid */
		const void * data_ptr = NULL;
		struct shall_sbinfo_rw_read * file1 = NULL, * file2 = NULL;
		int next_header, dataflag, s_count, d_space, remain, used;
		int file1_length = 0, file2_length = 0, tot_len, rem_len;
		save = fi->sbi.rw.read;
		err = get_log_header(fi, &sh);
		if (err <= 0) goto out_restore;
		s_count = err;
		d_space = 0;
		next_header = sh.next_header;
		/* if file1 present, skip it but remember where it was */
		if (sh.flags & SHALL_LOG_FILE1) {
			err = read_structure(idh);
			if (err <= 0) goto out_restore;
			s_count += err;
			file1_length = le32_to_cpu(idh.fileid);
			d_space += 1 + file1_length; /* " [file]" */
			if (sh.operation) d_space += 2;
			svfile1 = fi->sbi.rw.read;
			file1 = &svfile1;
			err = shall_mark_read(fi, file1_length);
			if (err <= 0) goto out_restore;
			s_count += file1_length;
		}
		/* if file2 present, skip it but remember where it was */
		if (sh.flags & SHALL_LOG_FILE2) {
			err = read_structure(idh);
			if (err <= 0) goto out_restore;
			s_count += err;
			file2_length = le32_to_cpu(idh.fileid);
			d_space += 1 + file2_length; /* " [file]" */
			if (sh.operation) d_space += 2;
			svfile2 = fi->sbi.rw.read;
			file2 = &svfile2;
			err = shall_mark_read(fi, file2_length);
			if (err <= 0) goto out_restore;
			s_count += file2_length;
		}
		/* if other data is present, read and store it */
		dataflag = sh.flags & SHALL_LOG_DMASK;
		switch (dataflag) {
			case SHALL_LOG_ATTR :
				err = read_structure(dah);
				if (err <= 0) goto out_restore;
				data_ptr = &dah;
				s_count += err;
				break;
			case SHALL_LOG_REGION :
				err = read_structure(drh);
				if (err <= 0) goto out_restore;
				data_ptr = &drh;
				s_count += err;
				break;
			case SHALL_LOG_FILEID :
				err = read_structure(idh);
				if (err <= 0) goto out_restore;
				data_ptr = &idh;
				s_count += err;
				break;
			case SHALL_LOG_SIZE :
				err = read_structure(dsh);
				if (err <= 0) goto out_restore;
				data_ptr = &dsh;
				s_count += err;
				break;
			case SHALL_LOG_ACL :
				err = read_structure(dlh);
				if (err <= 0) goto out_restore;
				s_count += err;
				rem_len = le32_to_cpu(dlh.count)
					* sizeof(struct shall_devacl_entry);
				tot_len = sizeof(dlh) + rem_len;
				freeit = shall_kmalloc(fi, tot_len, GFP_KERNEL);
				if (! freeit) {
					err = -ENOMEM;
					goto out_restore;
				}
				memcpy(freeit, &dlh, sizeof(dlh));
				err = shall_read_data_kernel(
					fi, freeit + sizeof(dlh), rem_len);
				if (err <= 0) goto out_restore;
				s_count += err;
				data_ptr = freeit;
				break;
			case SHALL_LOG_XATTR :
				err = read_structure(dxh);
				if (err <= 0) goto out_restore;
				s_count += err;
				rem_len = le32_to_cpu(dxh.namelen)
					+ le32_to_cpu(dxh.valuelen);
				tot_len = sizeof(dxh) + rem_len;
				freeit = shall_kmalloc(fi, tot_len, GFP_KERNEL);
				if (! freeit) {
					err = -ENOMEM;
					goto out_restore;
				}
				memcpy(freeit, &dxh, sizeof(dxh));
				err = shall_read_data_kernel(
					fi, freeit + sizeof(dxh), rem_len);
				if (err <= 0) goto out_restore;
				s_count += err;
				data_ptr = freeit;
				break;
			case SHALL_LOG_HASH :
				err = read_structure(dhh);
				if (err <= 0) goto out_restore;
				data_ptr = &dhh;
				s_count += err;
				break;
			case SHALL_LOG_DATA :
				err = read_structure(drh);
				if (err <= 0) goto out_restore;
				data_ptr = &drh;
				s_count += err;
				rem_len = le32_to_cpu(drh.length);
				tot_len = sizeof(drh) + rem_len;
				freeit = shall_kmalloc(fi, tot_len, GFP_KERNEL);
				if (! freeit) {
					err = -ENOMEM;
					goto out_restore;
				}
				err = shall_read_data_kernel(
					fi, freeit + sizeof(drh), rem_len);
				if (err <= 0) goto out_restore;
				s_count += err;
				data_ptr = freeit;
				break;
		}
		/* now try to print log */
		if (space < d_space + 10) goto out_nospace;
		remain = space - d_space - 1;
		used = print_log(buffer, remain, sh.operation, sh.result,
				 sh.flags, &sh.requested, data_ptr);
		if (used < 0) goto out_restore;
		if (used >= remain) goto out_nospace;
		/* if we need to store files, do so */
		svtemp = fi->sbi.rw.read;
		if (file1) {
			fi->sbi.rw.read = *file1;
			add_userstring(sh.operation ? " [" : " ");
			err = shall_read_data_user(fi, buffer + used,
						   file1_length);
			if (err <= 0) goto out_restore;
			used += file1_length;
			if (sh.operation) add_userstring("]");
			fi->sbi.rw.read = svtemp;
		}
		if (file2) {
			fi->sbi.rw.read = *file2;
			add_userstring(" [");
			err = shall_read_data_user(fi, buffer + used,
						   file2_length);
			if (err <= 0) goto out_restore;
			used += file2_length;
			add_userstring("]");
			fi->sbi.rw.read = svtemp;
		}
		add_userstring("\n");
		/* now advance pointers */
		if (s_count < next_header) {
			int skip = next_header - s_count;
			err = shall_mark_read(fi, skip);
			if (err <= 0) goto out_restore;
		}
		done += used;
		buffer += used;
		if (freeit) {
			kfree(freeit);
			freeit = NULL;
		}
	}
	atomic_set(&fi->sbi.ro.some_data,
		   fi->sbi.rw.read.data_length >=
		   	sizeof(struct shall_devheader));
	/* if we are in an overflow situation, try to log a recovery event */
	shall_log_recovery(fi);
	/* if anybody was waiting for space... let them try */
	wake_up_all(&fi->lq.log_queue);
	mutex_unlock(&fi->sbi.mutex);
	if (freeit) kfree(freeit);
	return done;
out_nospace:
	err = -EFBIG;
	goto out_restore;
out_fault:
	err = -EFAULT;
out_restore:
	fi->sbi.rw.read = save;
	atomic_set(&fi->sbi.ro.some_data,
		   fi->sbi.rw.read.data_length >=
		   	sizeof(struct shall_devheader));
	if (done > 0) {
		/* if we are in an overflow situation, try to log a
		 * recovery event */
		shall_log_recovery(fi);
		/* if anybody was waiting for space... let them try */
		wake_up_all(&fi->lq.log_queue);
	}
	mutex_unlock(&fi->sbi.mutex);
	if (freeit) kfree(freeit);
	return done > 0 ? done : err;
}
#undef add_userstring
#endif

