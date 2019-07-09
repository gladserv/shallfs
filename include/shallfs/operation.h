/* include/shallfs/operation.h
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * this file is part of SHALLFS
 *
 * Copyright (c) 2017-2019 Claudio Calvelli <shallfs@gladserv.com>
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

#ifndef _SHALL_OPERATION_H
#define _SHALL_OPERATION_H

enum shall_operation {
	SHALL_MOUNT	= 0x01,
	SHALL_REMOUNT,
	SHALL_UMOUNT,

	SHALL_OVERFLOW,
	SHALL_RECOVER,
	SHALL_TOO_BIG,

	SHALL_META,
	SHALL_MKNOD,
	SHALL_MKDIR,
	SHALL_LINK,
	SHALL_SYMLINK,
	SHALL_CREATE,
	SHALL_DELETE,
	SHALL_RMDIR,
	SHALL_OPEN,
	SHALL_WRITE,
	SHALL_COMMIT,
	SHALL_CLOSE,
	SHALL_MOVE,
	SHALL_SWAP,
	SHALL_SET_ACL,
	SHALL_SET_XATTR,
	SHALL_DEL_XATTR,

	SHALL_USERLOG,

	SHALL_MAX_OPCODE
};

/* log flags determine what data comes with which log */
enum shall_log_flags {
	SHALL_LOG_NODATA	= 0x0000,	/* no data present */
	SHALL_LOG_FILE1		= 0x0001,	/* file1 present */
	SHALL_LOG_FILE2		= 0x0002,	/* file2 present */
	SHALL_LOG_FILEID	= 0x0100,	/* fileid present */
	SHALL_LOG_ATTR		= 0x0200,	/* attr present */
	SHALL_LOG_XATTR		= 0x0400,	/* extended attribute present */
	SHALL_LOG_REGION	= 0x0800,	/* region present */
	SHALL_LOG_SIZE		= 0x1000,	/* size present */
	SHALL_LOG_ACL		= 0x2000,	/* ACL present */
	SHALL_LOG_HASH          = 0x4000,       /* hash of data present */
	SHALL_LOG_DATA          = 0x8000,       /* full data present */
	SHALL_LOG_DMASK		= 0xff00,	/* mask to get data type */
};

#define SHALL_HEADER_MAGIC 0x4c4a4853

struct shall_region {
	off_t start;
	size_t length;
	unsigned int fileid;
};

enum shall_attr_flags {
	shall_attr_mode    =  0x00000001,   /* mode has changed */
	shall_attr_user    =  0x00000002,   /* user has changed */
	shall_attr_group   =  0x00000004,   /* group has changed */
	shall_attr_block   =  0x00000008,   /* block device created */
	shall_attr_char    =  0x00000010,   /* character device created */
	shall_attr_size    =  0x00000020,   /* truncate / allocate operation */
	shall_attr_atime   =  0x00000040,   /* access time changed */
	shall_attr_mtime   =  0x00000080,   /* modification time changed */
	shall_attr_excl    =  0x00000100,   /* CREATE had O_EXCL */
};

struct shall_attr {
	enum shall_attr_flags flags;    /* for META: bitmap of what changed;
					 * for MKNOD: what data was provided */
	mode_t mode;                    /* file mode/permissions */
	uid_t user;
	gid_t group;
	union {
		dev_t device;           /* for MKNOD only */
		off_t size;             /* for META, if the operation
					 *           was truncate */
	};
	/* we need to work around some kernels which use timespec64 vs timespec
	 * and we can't make this dependent on kernel version because it's
	 * shared with userspace */
	time_t atime_sec;
	long atime_nsec;
	time_t mtime_sec;
	long mtime_nsec;
};

/* ACL flags; the actual ACL data is provided using the system's ACL
 * representation */
enum shall_acl_flags {
	shall_acl_default	= 0x0001,
	shall_acl_access	= 0x0002
};

#endif /* _SHALL_OPERATION_H */
