/*
 *  shall/opdata.h
 */

/* this file is always included by the userspace libraries; the kernel
 * code uses it if we asked for debugging */

#ifndef _SHALLFS_OPDATA_H_
#define _SHALLFS_OPDATA_H_ 1

struct shall_opdata {
	const char * name;
	int num_files;
	enum shall_log_flags has_data;
};

const struct shall_opdata shall_opdata[SHALL_MAX_OPCODE] = {
	[SHALL_MOUNT]		= { "MOUNT",     1, SHALL_LOG_NODATA },
	[SHALL_REMOUNT]		= { "REMOUNT",   1, SHALL_LOG_NODATA },
	[SHALL_UMOUNT]		= { "UMOUNT",    0, SHALL_LOG_NODATA },

	[SHALL_OVERFLOW]	= { "OVERFLOW",  0, SHALL_LOG_NODATA },
	[SHALL_RECOVER]		= { "RECOVER",   0, SHALL_LOG_SIZE },
	[SHALL_TOO_BIG]		= { "TOO_BIG",   0, SHALL_LOG_SIZE },

	[SHALL_META]		= { "META",      1, SHALL_LOG_ATTR },
	[SHALL_MKNOD]		= { "MKNOD",     1, SHALL_LOG_ATTR },
	[SHALL_MKDIR]		= { "MKDIR",     1, SHALL_LOG_ATTR },
	[SHALL_LINK]		= { "LINK",      2, SHALL_LOG_NODATA },
	[SHALL_SYMLINK]		= { "SYMLINK",   2, SHALL_LOG_ATTR },
	[SHALL_CREATE]		= { "CREATE",    1, SHALL_LOG_ATTR },
	[SHALL_DELETE]		= { "DELETE",    1, SHALL_LOG_NODATA },
	[SHALL_RMDIR]		= { "RMDIR",     1, SHALL_LOG_NODATA },
	[SHALL_OPEN]		= { "OPEN",      1, SHALL_LOG_FILEID },
	[SHALL_WRITE]		= { "WRITE",     0, SHALL_LOG_REGION },
	[SHALL_COMMIT]		= { "COMMIT",    0, SHALL_LOG_FILEID },
	[SHALL_CLOSE]		= { "CLOSE",     0, SHALL_LOG_FILEID },
	[SHALL_MOVE]		= { "MOVE",      2, SHALL_LOG_NODATA },
	[SHALL_SWAP]		= { "SWAP",      2, SHALL_LOG_NODATA },
	[SHALL_SET_ACL]		= { "SET_ACL",   1, SHALL_LOG_ACL },
	[SHALL_SET_XATTR]	= { "SET_XATTR", 1, SHALL_LOG_XATTR },
	[SHALL_DEL_XATTR]	= { "DEL_XATTR", 1, SHALL_LOG_XATTR },
};

#endif /* _SHALLFS_OPDATA_H_ */
