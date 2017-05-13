#ifndef _SHALL_LOG_H_
#define _SHALL_LOG_H_

#include <linux/posix_acl.h>
#ifdef CONFIG_SHALL_FS_DEBUG
#include <linux/slab.h>
#endif

/* commit any pending logs and wait for that to happen, then run the code
 * and finally unlock the queue */
void shall_commit_logs(struct shall_fsinfo *, void(*)(void *), void *);

/* run the commit thread; meant to be called during mount only */
int shall_commit_thread(void *);

/* log an event with 0 filenames and no other data */
int shall_log_0n(struct shall_fsinfo *, int operation, int result);

/* log an event with 0 filenames and integer data (fileid) */
int shall_log_0i(struct shall_fsinfo *, int operation,
		 int fileid, int result);

/* log an event with 0 filenames and a region structure */
int shall_log_0r(struct shall_fsinfo *, int operation,
		 loff_t start, size_t length, int fileid, int result);

/* log an event with 0 filenames and hash of data changed */
int shall_log_0h(struct shall_fsinfo *, int operation,
		 loff_t start, size_t length, const char __user *data,
		 int fileid, int result);

/* log an event with 0 filenames and copy of data changed */
int shall_log_0d(struct shall_fsinfo *, int operation,
		 loff_t start, size_t length, const char __user *data,
		 int fileid, int result);

/* log an event with 1 filename and no other data */
int shall_log_1n(struct shall_fsinfo *, int operation,
		 const char *, int result);

/* log an event with 1 filename and integer data (fileid) */
int shall_log_1i(struct shall_fsinfo *, int operation,
		 const char *, int fileid, int result);

/* log an event with 1 filename and an "attr" structure */
int shall_log_1a(struct shall_fsinfo *, int operation,
		 const char *, const struct shall_attr *, int result);

/* log an event with 1 filename and a POSIX acl */
int shall_log_1l(struct shall_fsinfo *, int operation, const char *,
		 int access, const struct posix_acl *, int result);

/* log an event with 1 filename and a POSIX extended attribute */
int shall_log_1x(struct shall_fsinfo *, int operation, const char *file,
		 const char *attr, const void *value, size_t size,
		 int flags, int result);

/* log an event with 2 filenames and no other data */
int shall_log_2n(struct shall_fsinfo *, int operation,
		 const char *, const char *, int result);

/* log an event with 2 filenames and an "attr" structure */
int shall_log_2a(struct shall_fsinfo *, int operation,
		 const char *name1, const char *name2,
		 const struct shall_attr *, int result);

/* log a debug event */
#ifdef CONFIG_SHALL_FS_DEBUG
#define shall_log_debug(fi, message) \
	if (IS_DEBUG((fi))) \
		shall_log_2n((fi), 0, (message), __FILE__, __LINE__)

/* "debugging" version of memory allocation functions */
static __always_inline void *__shall_kmalloc(struct shall_fsinfo *fi,
					     size_t size, gfp_t flags,
					     const char * file, int line)
{
	void * ptr = kmalloc(size, flags);
	if (ptr && fi && IS_DEBUG(fi)) {
		char buf[32];
		snprintf(buf, sizeof(buf), "kmalloc(%ld)=%p",
			 (long)size, ptr);
		shall_log_2n(fi, 0, buf, file, line);
	}
	return ptr;
}
static __always_inline void *__shall_kstrdup(struct shall_fsinfo *fi,
					     const char * str, gfp_t flags,
					     const char * file, int line)
{
	void * ptr = kstrdup(str, flags);
	if (ptr && fi && IS_DEBUG(fi)) {
		char buf[32];
		snprintf(buf, sizeof(buf), "kstrdup(%d)=%p",
			 (int)strlen(str), ptr);
		shall_log_2n(fi, 0, buf, file, line);
	}
	return ptr;
}
static __always_inline void __shall_kfree(struct shall_fsinfo *fi, void *ptr,
					  const char * file, int line)
{
	if (fi && IS_DEBUG(fi)) {
		char buf[32];
		snprintf(buf, sizeof(buf), "kfree(%p)", ptr);
		shall_log_2n(fi, 0, buf, file, line);
	}
	kfree(ptr);
}
static __always_inline void *__shall_vmalloc(struct shall_fsinfo *fi,
					     size_t size,
					     const char * file, int line)
{
	void * ptr = vmalloc(size);
	if (ptr && fi && IS_DEBUG(fi)) {
		char buf[32];
		snprintf(buf, sizeof(buf), "vmalloc(%ld)=%p",
			 (long)size, ptr);
		shall_log_2n(fi, 0, buf, file, line);
	}
	return ptr;
}
static __always_inline void __shall_vfree(struct shall_fsinfo *fi, void *ptr,
					  const char * file, int line)
{
	if (fi && IS_DEBUG(fi)) {
		char buf[32];
		snprintf(buf, sizeof(buf), "vfree(%p)", ptr);
		shall_log_2n(fi, 0, buf, file, line);
	}
	vfree(ptr);
}
static __always_inline char *__shall_getname(struct shall_fsinfo *fi,
					     const char * file, int line)
{
	char * ptr = __getname();
	if (ptr && fi && IS_DEBUG(fi)) {
		char buf[32];
		snprintf(buf, sizeof(buf), "getname(%d)=%p",
		         PATH_MAX, (void *)ptr);
		shall_log_2n(fi, 0, buf, file, line);
	}
	return ptr;
}
static __always_inline void __shall_putname(struct shall_fsinfo *fi, char *ptr,
					    const char * file, int line)
{
	if (fi && IS_DEBUG(fi)) {
		char buf[32];
		snprintf(buf, sizeof(buf), "putname(%p)", ptr);
		shall_log_2n(fi, 0, buf, file, line);
	}
	__putname(ptr);
}
#define shall_kmalloc(fi, size, gfp) \
	__shall_kmalloc((fi), (size), (gfp), __FILE__, __LINE__)
#define shall_kstrdup(fi, str, gfp) \
	__shall_kstrdup((fi), (str), (gfp), __FILE__, __LINE__)
#define shall_kfree(fi, ptr) \
	__shall_kfree((fi), (ptr), __FILE__, __LINE__)
#define shall_vmalloc(fi, size) \
	__shall_vmalloc((fi), (size), __FILE__, __LINE__)
#define shall_vfree(fi, ptr) \
	__shall_vfree((fi), (ptr), __FILE__, __LINE__)
#define shall_getname(fi) \
	__shall_getname((fi), __FILE__, __LINE__)
#define shall_putname(fi, ptr) \
	__shall_putname((fi), (ptr), __FILE__, __LINE__)
#else
#define shall_log_debug(fi, message)
#define shall_kmalloc(fi, size, gfp) kmalloc((size), (gfp))
#define shall_kstrdup(fi, str, gfp) kstrdup((str), (gfp))
#define shall_kfree(fi, ptr) kfree((ptr))
#define shall_vmalloc(fi, size) vmalloc((size))
#define shall_vfree(fi, ptr) vfree((ptr))
#define shall_getname(fi) __getname()
#define shall_putname(fi, ptr) __putname((ptr))
#endif

/* retrieves logs from journal and/or memory buffer and store it in the
 * memory area provided; returns the amount of buffer actually used,
 * which may be 0 if there was nothing available, or negative if an
 * error occurred */
ssize_t shall_bin_logs(struct shall_fsinfo *, char __user *, size_t);

/* removes logs from journal without storing them anywhere; caller must
 * not already hold the mutex */
int shall_delete_logs(struct shall_fsinfo *, size_t);

#ifdef CONFIG_SHALL_FS_DEBUG
/* similar to shall_bin_logs, but produces a printable version */
ssize_t shall_print_logs(struct shall_fsinfo *, char __user *, size_t);
#endif

#endif /* _SHALL_LOG_H_ */
