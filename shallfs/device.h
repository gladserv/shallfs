/* shallfs/device.h
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

#ifndef _SHALL_INTERNAL_DEVICE_H_
#define _SHALL_INTERNAL_DEVICE_H_

/* calculate location of n-th superblock as multiple of device block */
static inline sector_t shall_superblock_location(int n) {
	return (sector_t)n * 4 * (4 * (sector_t)n + 1);
}

/* calculate block containing some data given the ring buffer offset and
 * the total number of superblocks */
void shall_calculate_block(loff_t, int, struct shall_devptr *);

/* read n-th superblock; caller needs to either hold the mutex, or
 * call this during mount before there can be any operation */
int shall_read_superblock(struct shall_fsinfo *, int n, int silent);

/* read a block of data from device or commit buffer, and mark the
 * corresponding area on the device as unused; there are two versions of
 * this, depending on whether the destination is user or kernel space;
 * caller must hold the mutex locked */
ssize_t shall_read_data_kernel(struct shall_fsinfo *, void *, size_t);
ssize_t shall_read_data_user(struct shall_fsinfo *, void __user *, size_t);

/* mark some data as read without actually reading it;  this is about the
 * same as:
 *     char buffer[len];
 *     shall_read_data_kernel(fi, buffer, len);
 * except that it does not need to allocate any buffers
 */
ssize_t shall_mark_read(struct shall_fsinfo *, size_t);

/* write n-th superblock; caller needs to either hold the mutex, or
 * call this during umount after all operations complete */
int shall_write_superblock(const struct shall_fsinfo *, int n, int sync);

/* write commit buffer to device; can be called with the mutex locked
 * or unlocked, but the caller needs to say what */
int shall_write_data(struct shall_fsinfo *, int locked, int why, int sync);

#endif /* _SHALL_INTERNAL_DEVICE_H_ */
