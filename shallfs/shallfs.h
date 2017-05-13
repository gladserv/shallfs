/*
 *  linux/fs/shallfs/shallfs.h
 *
 *  some code inspired by various Linux filesystems, particularly overlyfs,
 *  which is Copyright (C) 2011 Novell Inc.
 */

#ifndef _SHALL_SHALL_H_
#define _SHALL_SHALL_H_ 1

enum shall_flags {
	OVERFLOW_DROP	= 0x0000,
	OVERFLOW_WAIT	= 0x0001,
	OVERFLOW_MASK	= OVERFLOW_DROP | OVERFLOW_WAIT,

	LOG_BEFORE	= 0x0002,
	LOG_AFTER	= 0x0004,
	LOG_TWICE	= LOG_BEFORE | LOG_AFTER,
	LOG_MASK	= LOG_TWICE,

	TOO_BIG_LOG	= 0x0000,
	TOO_BIG_ERROR	= 0x0008,
	TOO_BIG_MASK	= TOO_BIG_LOG | TOO_BIG_ERROR,

	DATA_NONE       = 0x0000,
	DATA_HASH       = 0x0010,
	DATA_FULL       = 0x0020,
	DATA_MASK       = DATA_HASH | DATA_FULL,

#ifdef CONFIG_SHALL_FS_DEBUG
	DEBUG_OFF       = 0x0000,
	DEBUG_ON        = 0x1000,
	DEBUG_MASK      = DEBUG_OFF | DEBUG_ON,

	NAME_OFF        = 0x0000,
	NAME_ON         = 0x2000,
	NAME_MASK       = NAME_OFF | NAME_ON,
#endif
};

/* some handy macros to avoid a long line just to test for drop/wait */
#define IS_DROP(fi) (((fi)->options.flags & OVERFLOW_MASK) == OVERFLOW_DROP)
#define IS_WAIT(fi) (((fi)->options.flags & OVERFLOW_MASK) != OVERFLOW_DROP)

#define IS_DROP_O(opt) (((opt).flags & OVERFLOW_MASK) == OVERFLOW_DROP)
#define IS_WAIT_O(opt) (((opt).flags & OVERFLOW_MASK) != OVERFLOW_DROP)

/* some handy macros to avoid a long line just to test for log/error */
#define IS_LOG(fi) (((fi)->options.flags & TOO_BIG_MASK) == TOO_BIG_LOG)
#define IS_ERROR(fi) (((fi)->options.flags & TOO_BIG_MASK) != TOO_BIG_LOG)

/* some handy macros to decide whether to log */
#define IS_LOG_BEFORE(fi) ((fi)->options.flags & LOG_BEFORE)
#define IS_LOG_AFTER(fi) ((fi)->options.flags & LOG_AFTER)

/* some handy macros to decide whether to print debugging info */
#ifdef CONFIG_SHALL_FS_DEBUG
#define IS_DEBUG(fi) (((fi)->options.flags & DEBUG_MASK) == DEBUG_ON)
#define SHOW_NAME(fi) (((fi)->options.flags & NAME_MASK) == NAME_ON)
#else
#define SHOW_NAME(fi) 1
#endif

/* structure used to store a physical block number and offset within the
 * block: also used to store the number of blocks available for data */
struct shall_devptr {
	sector_t block;			/* actual block number */
	sector_t next_super;		/* block with next superblock */
	int offset;			/* offset within block */
	int n_super;			/* number of superblocks before
					 * this physical block */
};

/* filesystem mount options; since the can only be changed on remount,
 * these can be accessed without any locking; the exception is the
 * commit thread, which may try to access this while it is being
 * changed: to protect from that, it'll only access this data while
 * holding the superblock info mutex (see below), and the remount call
 * will make sure to hold the mutex while updating the mount options */
struct shall_options {
	char * fspath;
	char * pathfilter;
	int pathfilter_count;
	int commit_seconds;
	int commit_size;
	enum shall_flags flags;
	char * data;
};

/* read-only part of the superblock information; this can only be changed
 * by unmounting and remounting, so we can just access it without any
 * locking (we have the wait queue here because it does its own locking) */
struct shall_sbinfo_ro {
	struct timespec mounted;	/* when was this mounted */
	loff_t device_size;		/* total size of device */
	loff_t data_space;		/* space available for data */
	int num_superblocks;		/* total number of superblocks */
	int log_alignment;		/* log alignment */
	enum shall_sb_flags flags;	/* superblock flags */
	struct shall_devptr maxptr;	/* cached calculation see device.c */
	wait_queue_head_t data_queue;	/* processes waiting for data */
	/* the following are used by /proc/shallfs/<device>/logs etc;
	 * see fs/shallfs/proc.c for details: they are here because they
	 * can be accessed without locking */
	atomic_t logs_reading;
	atomic_t logs_writing;
	atomic_t logs_valid;
	/* the following are used to control whether the commit thread is
	 * running, and to check for commits in progress; using atomic_t
	 * allows us to avoid some locks */
	atomic_t allow_commit_thread;
	atomic_t inside_commit;
	/* the following is a "quick check" for data available without
	 * needing to lock the mutex to access the rw structure (see
	 * below); we set some_data when adding any data, and we
	 * clear it when we remove the last log from the journal */
	atomic_t some_data;
	/* quick test for commit thread actually running; this is because
	 * if we call kthread_stop() after it dies we get a panic (not
	 * supposed to happen looking at the kernel sources, but I did
	 * see that happening */
	atomic_t thread_running;
};

/* the read-write part of the superblock information is further split
 * into two parts: things which can change when reading logs (read)
 * and things which can only change when doing any other operation (other);
 * this is because shall_get_logs and shall_print_logs may need to save
 * and restore the "read" part in case of error, and it's handy to be
 * able to access just that in a single blob; no matter what part one
 * accesses, the mutex in shall_sbinfo must be locked */
struct shall_sbinfo_rw_read {
	loff_t data_start;		/* start of journal */
	loff_t data_length;		/* current size of journal */
	loff_t committed;		/* size of committed data */
	struct shall_devptr startptr;	/* block containing data_start */
	struct shall_devptr commitptr;	/* block where next commit happens */
	int buffer_written;		/* size of data in commit buffer */
	int buffer_read;		/* any buffered data which has already
					 * been discarded */
};

struct shall_sbinfo_rw_other {
	time_t last_commit;		/* time of last commit */
	int last_sb_written;		/* last superblock updated */
	loff_t max_length;		/* maximum size of journal, this
					 * is the maximum value of
					 * data_length we observed */
	int64_t version;		/* increased on each commit */
	int logged;			/* number of operations logged */
	int commit_count[3];		/* number of commits of different type:
					 * 0 = size exceeded,
					 * 1 = time exceeded,
					 * 2 = forced commit by remount etc */
	char * commit_buffer;		/* current commit buffer */
};

struct shall_sbinfo_rw {
	struct shall_sbinfo_rw_read read;
	struct shall_sbinfo_rw_other other;
};

/* handy structure to contain both parts of the superblock information */
struct shall_sbinfo {
	struct mutex mutex;		/* use this to access "rw" */
	struct shall_sbinfo_rw rw;
	struct shall_sbinfo_ro ro;
};

/* communication between processes waiting for log space and processes
 * providing the space is via a wait queue; any variable in the following
 * structure is used in conditional waits, and must be protected by
 * acquiring the wait queue's lock; if we need both the superblock info
 * mutex and the queue lock, make sure to acquire the mutex first, and
 * release it last, to avoid deadlock; but ideally try not to hold them
 * both at the same time */
struct shall_logqueue {
	wait_queue_head_t log_queue;	/* processes waiting for space */
	loff_t extra_space;		/* space required... */
	int num_dropped;		/* operations dropped so far */
};

struct shall_fsinfo {
	struct shall_options options;	/* mount options */
	struct shall_sbinfo sbi;	/* superblock information */
	struct shall_logqueue lq;	/* waiting... */
	struct super_block * sb;	/* kernel's fs superblock */
	struct task_struct * commit_thread; /* the commit thread */
	struct proc_dir_entry * proc;	/* /proc/shallfs/<device> */
	struct path root_path;		/* underlying fs */
	struct vfsmount * mount;	/* underlying fs */
	struct shall_fsinfo * prev;	/* linked list of mounted filesystems */
	struct shall_fsinfo * next;	/* linked list of mounted filesystems */
};

#endif /* _SHALL_SHALL_H_ */
