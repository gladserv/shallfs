/* this file is always included by the userspace libraries; the kernel
 * code uses it if we asked for debugging
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

	[SHALL_USERLOG]		= { "USER_LOG",  1, SHALL_LOG_NODATA },
};

#endif /* _SHALLFS_OPDATA_H_ */
