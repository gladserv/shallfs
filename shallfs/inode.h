/* shallfs/inode.h
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

#ifndef _SHALL_INODE_H
#define _SHALL_INODE_H

struct inode * shall_new_inode(struct super_block *, struct dentry *);

void shall_evict_inode(struct inode *);

extern const struct xattr_handler shall_xattr_handler;
#endif /* _SHALL_INODE_H */
