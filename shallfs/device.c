/*
 *  linux/fs/shallfs/device.c
 */

#include <linux/module.h>
#include <linux/string.h>
#include <linux/namei.h>
#include <linux/time.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>
#include <linux/crc32.h>
#include <linux/buffer_head.h>
#include <linux/atomic.h>
#include <linux/kthread.h>
#include <shallfs/operation.h>
#include <shallfs/device.h>
#include "shallfs.h"
#include "super.h"
#include "log.h"
#include "device.h"

/* calculate checksum on a superblock; since all on-device data is
 * little-endian, we use crc32_le */
#define checksum_super(ds) \
	crc32_le(0x4c414853, (void *)&(ds), shall_superblock_checksize)

/* read n-th superblock */
int shall_read_superblock(struct shall_fsinfo *fi, int n, int silent) {
	struct shall_devsuper ds;
	struct buffer_head * bh;
#ifdef CONFIG_SHALL_FS_DEBUG
	const char * reason = "";
#define give_up(n) { reason = (n); goto invalid_sb; }
#else
#define give_up(n) goto invalid_sb;
#endif
	int chk;
	/* try to read it */
	bh = sb_bread(fi->sb, shall_superblock_location(n));
	if (! bh) return -EIO;
	memcpy(&ds, bh->b_data + SHALL_SB_OFFSET, sizeof(ds));
	brelse(bh);
	/* check: checksum is valid */
	chk = checksum_super(ds);
	if (chk != le32_to_cpu(ds.checksum))
		give_up("Wrong checksum");
	/* check: this_superblock is correct */
	if (le32_to_cpu(ds.this_superblock) != n)
		give_up("Inconsistent superblock numeber");
	/* check: both magic strings present */
	if (strncmp(ds.magic1, SHALL_SB_MAGIC, sizeof(ds.magic1)) != 0)
		give_up("wrong magic #1");
	if (strncmp(ds.magic2, SHALL_SB_MAGIC, sizeof(ds.magic2)) != 0)
		give_up("wrong magic #2");
	/* check: flags contains SHALL_SB_VALID */
	fi->sbi.ro.flags = le32_to_cpu(ds.flags);
	if (! (fi->sbi.ro.flags & SHALL_SB_VALID))
		give_up("no SHALL_SB_VALID in flags");
	/* check: device_size is <= the physical size of the device */
	fi->sbi.ro.device_size = le64_to_cpu(ds.device_size);
	if (fi->sbi.ro.device_size > fi->sb->s_bdev->bd_inode->i_size)
		give_up("device_size > physical size of device");
	/* check: device_size is a multiple of SHALL_DEV_BLOCK and >= 65536 */
	if (fi->sbi.ro.device_size % SHALL_DEV_BLOCK)
		give_up("device_size not a multiple of SHALL_DEV_BLOCK");
	if (fi->sbi.ro.device_size < 65536)
		give_up("device_size < 65536");
	/* check: num_superblocks > 8 */
	fi->sbi.ro.num_superblocks = le32_to_cpu(ds.num_superblocks);
	if (fi->sbi.ro.num_superblocks <= 8)
		give_up("num_superblocks <= 8");
	/* check: data_space + SHALL_DEV_BLOCK * num_superblocks
	 *        == device_size */
	fi->sbi.ro.data_space = le64_to_cpu(ds.data_space);
	if (fi->sbi.ro.data_space +
	    SHALL_DEV_BLOCK * fi->sbi.ro.num_superblocks !=
	    fi->sbi.ro.device_size)
		give_up("data_space * SHALL_DEV_BLOCK * num_superblocks "
			"!= device_size");
	/* check: 0 <= data_start < data_space */
	fi->sbi.rw.read.data_start = le64_to_cpu(ds.data_start);
	if (fi->sbi.rw.read.data_start < 0)
		give_up("data_start < 0");
	if (fi->sbi.rw.read.data_start >= fi->sbi.ro.data_space)
		give_up("data_start >= data_space");
	/* check: 0 <= data_length <= data_space */
	fi->sbi.rw.read.data_length = le64_to_cpu(ds.data_length);
	if (fi->sbi.rw.read.data_length < 0)
		give_up("data_length < 0");
	if (fi->sbi.rw.read.data_length > fi->sbi.ro.data_space)
		give_up("data_length > data_space");
	/* check: data_length <= max_length <= data_space */
	fi->sbi.rw.other.max_length = le64_to_cpu(ds.max_length);
	if (fi->sbi.rw.other.max_length < fi->sbi.rw.read.data_length)
		give_up("max_length < data_length");
	if (fi->sbi.rw.other.max_length > fi->sbi.ro.data_space)
		give_up("max_length > data_space");
	/* check: alignment is a multiple of 8 and >= 8 */
	fi->sbi.ro.log_alignment = le32_to_cpu(ds.alignment);
	if (fi->sbi.ro.log_alignment % 8)
		give_up("alignment not a multiple of 8");
	if (fi->sbi.ro.log_alignment < 8)
		give_up("alignment < 8");
	/* check: location(last superblock) + sizeof(sb) <= device_size */
	if (SHALL_DEV_BLOCK *
	    shall_superblock_location(fi->sbi.ro.num_superblocks - 1) +
	    sizeof(ds) >= fi->sbi.ro.device_size)
		give_up("location of last superblock past end of device");
	/* all seems OK */
	fi->sbi.rw.other.version = le64_to_cpu(ds.version);
	return 0;
invalid_sb:
	if (silent) return -EINVAL;
#ifdef CONFIG_SHALL_FS_DEBUG
	printk(KERN_ERR "Invalid superblock #%d (%s)\n", n, reason);
	printk(KERN_ERR "    magic1=<%.*s>\n",
	       (int)sizeof(ds.magic1), ds.magic1);
	printk(KERN_ERR "    device_size=%lld (%lld)\n",
	       (long long)le64_to_cpu(ds.device_size),
	       (long long)fi->sbi.ro.device_size);
	printk(KERN_ERR "    data_space=%lld\n",
	       (long long)le64_to_cpu(ds.data_space));
	printk(KERN_ERR "    data_start=%lld\n",
	       (long long)le64_to_cpu(ds.data_start));
	printk(KERN_ERR "    data_length=%lld\n",
	       (long long)le64_to_cpu(ds.data_length));
	printk(KERN_ERR "    max_length=%lld\n",
	       (long long)le64_to_cpu(ds.max_length));
	printk(KERN_ERR "    version=%lld\n",
	       (long long)le64_to_cpu(ds.version));
	printk(KERN_ERR "    flags=%x\n",
	       le32_to_cpu(ds.flags));
	printk(KERN_ERR "    alignment=%d\n",
	       le32_to_cpu(ds.alignment));
	printk(KERN_ERR "    num_superblocks=%d\n",
	       le32_to_cpu(ds.num_superblocks));
	printk(KERN_ERR "    this_superblock=%d\n",
	       le32_to_cpu(ds.this_superblock));
	printk(KERN_ERR "    magic2=<%.*s>\n",
	       (int)sizeof(ds.magic2), ds.magic2);
#else
	printk(KERN_ERR "Invalid superblock #%d\n", n);
#endif
	return -EINVAL;
}

/* write n-th superblock */
int shall_write_superblock(const struct shall_fsinfo *fi, int n, int sync) {
	struct shall_devsuper ds;
	struct buffer_head * bh;
	memset(&ds, 0, sizeof(ds));
	strncpy(ds.magic1, SHALL_SB_MAGIC, sizeof(ds.magic1));
	ds.device_size = cpu_to_le64(fi->sbi.ro.device_size);
	ds.data_space = cpu_to_le64(fi->sbi.ro.data_space);
	ds.data_start = cpu_to_le64(fi->sbi.rw.read.data_start);
	ds.data_length = cpu_to_le64(fi->sbi.rw.read.data_length);
	ds.max_length = cpu_to_le64(fi->sbi.rw.other.max_length);
	ds.version = cpu_to_le64(fi->sbi.rw.other.version);
	ds.flags = cpu_to_le32(fi->sbi.ro.flags);
	ds.alignment = cpu_to_le32(fi->sbi.ro.log_alignment);
	ds.num_superblocks = cpu_to_le32(fi->sbi.ro.num_superblocks);
	ds.this_superblock = cpu_to_le32(n);
	ds.new_size = cpu_to_le64(0);
	ds.new_alignment = cpu_to_le32(0);
	ds.new_superblocks = cpu_to_le32(0);
	strncpy(ds.magic2, SHALL_SB_MAGIC, sizeof(ds.magic2));
	ds.checksum = cpu_to_le32(checksum_super(ds));
	bh = sb_bread(fi->sb, shall_superblock_location(n));
	if (! bh) return -EIO;
	memcpy(bh->b_data + SHALL_SB_OFFSET, &ds, sizeof(ds));
	mark_buffer_dirty(bh);
	if (sync) {
		int err = sync_dirty_buffer(bh);
		if (! err) {
			err = buffer_write_io_error(bh);
			if (err) {
				/* nothing we can do about this, but we clear
				 * the error so we don't keep reporting it
				 * again */
				clear_buffer_write_io_error(bh);
				set_buffer_uptodate(bh);
			}
		}
		if (err) {
			printk(KERN_ERR "Error syncing superblock %d: %d\n",
			       n, err);
			brelse(bh);
			return err;
		}
	} else {
		write_dirty_buffer(bh, WRITE);
	}
	brelse(bh);
	return 0;
}

/* calculate block containing some data given the ring buffer offset and
 * the total number of superblocks */
void shall_calculate_block(loff_t p, int ns, struct shall_devptr *b) {
	/* this does not need to be fast or clever: it is executed just
	 * twice for each mount */
	sector_t remain = p / SHALL_DEV_BLOCK, prev = 0, result = 1;
	int nsb = 1;
	b->offset = p % SHALL_DEV_BLOCK;
	while (nsb < ns && remain > 0) {
		sector_t this = shall_superblock_location(nsb);
		sector_t diff = this - prev - 1;
		/* there are "diff" blocks between this superblock and the
		 * next; if remain < diff, this is the place we need */
		if (remain < diff) break;
		remain -= diff;
		prev = this;
		result += diff + 1;
		nsb++;
	}
	b->block = result + remain;
	b->n_super = nsb;
	if (nsb < ns)
		b->next_super = shall_superblock_location(nsb);
	else
		b->next_super = 0;
}

/* increment physical block number, skipping next superblock if required */
static inline void inc_block(struct shall_devptr *b,
			     const struct shall_devptr *maxptr)
{
	b->block++;
	if (b->block >= maxptr->block) {
		b->block = 1;
		b->n_super = 1;
	}
	if (b->n_super < maxptr->n_super && b->block == b->next_super) {
		b->block++;
		b->n_super++;
		if (b->block >= maxptr->block) {
			b->block = 1;
			b->n_super = 1;
		}
	}
	if (b->n_super < maxptr->n_super)
		b->next_super = shall_superblock_location(b->n_super);
	else
		b->next_super = 0;
}

/* code for shall_read_data_*(), this is a macro so that we can make sure
 * the code for both is identical (apart for the actual copy to kernel or
 * user buffers) */
#define read_code(name, type, preif, copy, postif) \
ssize_t name(struct shall_fsinfo *fi, void type *_d, size_t len) { \
	char type * dest = _d; \
	size_t orig = len; \
	if (len < 1) return 0; \
	if (len > fi->sbi.rw.read.data_length) return 0; \
	fi->sbi.rw.read.data_length -= len; \
	/* first read any data which has already been committed */ \
	if (fi->sbi.rw.read.committed > 0) { \
		int offset = fi->sbi.rw.read.startptr.offset; \
		while (len > 0 && fi->sbi.rw.read.committed > 0) { \
			struct buffer_head * bh; \
			size_t todo = len; \
			if (todo > fi->sbi.rw.read.committed) \
				todo = fi->sbi.rw.read.committed; \
			if (todo + offset > SHALL_DEV_BLOCK) \
				todo = SHALL_DEV_BLOCK - offset; \
			bh = sb_bread(fi->sb, \
				      fi->sbi.rw.read.startptr.block); \
			if (! bh) return -EIO; \
			preif(copy(dest, bh->b_data + offset, todo)) postif; \
			fi->sbi.rw.read.data_start += todo; \
			if (fi->sbi.rw.read.data_start >= \
				fi->sbi.ro.data_space) \
					fi->sbi.rw.read.data_start -= \
						fi->sbi.ro.data_space; \
			len -= todo; \
			dest += todo; \
			fi->sbi.rw.read.committed -= todo; \
			offset += todo; \
			if (offset >= SHALL_DEV_BLOCK) { \
				offset -= SHALL_DEV_BLOCK; \
				inc_block(&fi->sbi.rw.read.startptr, \
					  &fi->sbi.ro.maxptr); \
			} \
		} \
		fi->sbi.rw.read.startptr.offset = offset; \
	} \
	if (len <= 0) return orig; \
	/* if we get here, we'll need to read some uncommitted data */ \
	preif(copy(dest, \
		   fi->sbi.rw.other.commit_buffer + \
		   	fi->sbi.rw.read.buffer_read, \
		   len)) \
		postif; \
	fi->sbi.rw.read.buffer_read += len; \
	/* we also need to adjust data_start even though we aren't writing \
	 * there */ \
	fi->sbi.rw.read.data_start += len; \
	if (fi->sbi.rw.read.data_start >= fi->sbi.ro.data_space) \
		fi->sbi.rw.read.data_start -=  fi->sbi.ro.data_space; \
	fi->sbi.rw.read.startptr.offset += len; \
	while (fi->sbi.rw.read.startptr.offset >= SHALL_DEV_BLOCK) { \
		fi->sbi.rw.read.startptr.offset -= SHALL_DEV_BLOCK; \
		inc_block(&fi->sbi.rw.read.startptr, \
			  &fi->sbi.ro.maxptr); \
	} \
	/* and ditto for the commit pointer */ \
	fi->sbi.rw.read.commitptr.offset += len; \
	while (fi->sbi.rw.read.commitptr.offset >= SHALL_DEV_BLOCK) { \
		fi->sbi.rw.read.commitptr.offset -= SHALL_DEV_BLOCK; \
		inc_block(&fi->sbi.rw.read.commitptr, \
			  &fi->sbi.ro.maxptr); \
	} \
	/* and if we happen to have read the whole buffer... */ \
	if (fi->sbi.rw.read.buffer_read >= fi->sbi.rw.read.buffer_written) { \
		fi->sbi.rw.read.buffer_read = 0; \
		fi->sbi.rw.read.buffer_written = 0; \
	} \
	return orig; \
}

/* read a block of data from device or commit buffer, and mark the
 * corresponding area on the device as unused; there are two versions of
 * this, depending on whether the destination is user or kernel space;
 * caller must hold the mutex locked */
read_code(shall_read_data_kernel, /* kernel */, /* no if */, memcpy, /* no */)
read_code(shall_read_data_user, __user, if, copy_to_user, return -EFAULT)

/* mark some data as read without actually reading it;  this is about the
 * same as:
 *     char buffer[len];
 *     shall_read_data_kernel(fi, buffer, len);
 * except that it does not need to allocate any buffers
 */
#define nullcpy(d, s, l) 0
static read_code(_mark_read, /* kernel */, if, nullcpy, /* nothing */; )

ssize_t shall_mark_read(struct shall_fsinfo *fi, size_t len) {
	if (len > fi->sbi.rw.read.data_length)
		len = fi->sbi.rw.read.data_length;
	return _mark_read(fi, NULL, len);
}

/* write commit buffer to device; can be called with the mutex locked
 * or unlocked, but the caller needs to say what */
int shall_write_data(struct shall_fsinfo *fi, int locked, int why, int sync) {
	int done = 0, err = 0;
	if (why < 0 || why > 2) return -EINVAL;
	while (1) {
		struct buffer_head * bh;
		const void * ptr;
		loff_t csize;
		sector_t block;
		size_t todo;
		int offset;
		if (! locked) mutex_lock(&fi->sbi.mutex);
		if (fi->sbi.rw.read.committed >= fi->sbi.rw.read.data_length) {
			/* all done */
			struct timespec now = current_fs_time(fi->sb);
			fi->sbi.rw.other.last_commit = now.tv_sec;
			fi->sbi.rw.read.buffer_read = 0;
			fi->sbi.rw.read.buffer_written = 0;
			if (done) {
				int n_sb = ++fi->sbi.rw.other.last_sb_written;
				fi->sbi.rw.other.version++;
				if (n_sb >= fi->sbi.ro.num_superblocks)
					n_sb = fi->sbi.rw.other.last_sb_written
						= 1;
				err = shall_write_superblock(fi, n_sb, sync);
			}
			if (! locked) mutex_unlock(&fi->sbi.mutex);
			break;
		}
		/* OK, try to commit another block or fraction thereof */
		offset = fi->sbi.rw.read.commitptr.offset;
		block = fi->sbi.rw.read.commitptr.block;
		todo = SHALL_DEV_BLOCK - offset;
		csize = fi->sbi.rw.read.data_length - fi->sbi.rw.read.committed;
		if (todo > csize) todo = csize;
		ptr = fi->sbi.rw.other.commit_buffer
		    + fi->sbi.rw.read.buffer_read;
		fi->sbi.rw.read.buffer_read += todo;
		fi->sbi.rw.read.commitptr.offset += todo;
		fi->sbi.rw.read.committed += todo;
		if (fi->sbi.rw.read.commitptr.offset >= SHALL_DEV_BLOCK) {
			fi->sbi.rw.read.commitptr.offset -= SHALL_DEV_BLOCK;
			inc_block(&fi->sbi.rw.read.commitptr,
				  &fi->sbi.ro.maxptr);
		}
		/* we now recorded that we've committed this block... if
		 * we were called without the mutex release it now and then
		 * go on and do the actual write: something else may then
		 * update the buffer, or even start another commit in
		 * parallel, but it'll all work; only thing which may be
		 * a problem is if a remount happens in the time we do
		 * the write, but that will wait for us to finish first */
		if (! locked) mutex_unlock(&fi->sbi.mutex);
		bh = sb_bread(fi->sb, block);
		if (! bh) {
			printk(KERN_ERR
			       "shallfs(%s): Cannot update block %lld\n",
			       fi->options.fspath, (long long)block);
			err = -EIO;
			break;
		}
		memcpy(bh->b_data + offset, ptr, todo);
		mark_buffer_dirty(bh);
		if (sync) {
			err = sync_dirty_buffer(bh);
			if (! err)
				err = buffer_write_io_error(bh);
			if (err) {
				printk(KERN_ERR
				       "Error writing block %lld: %d\n",
				       (long long)block, err);
				brelse(bh);
				return err;
			}
		} else {
			write_dirty_buffer(bh, WRITE);
		}
		brelse(bh);
		done = 1;
	}
	return err;
}

