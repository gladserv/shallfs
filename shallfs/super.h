/* shallfs/super.h
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

#ifndef _SHALL_SUPER_H_
#define _SHALL_SUPER_H_

/* update superblock from current data; note that the caller must
 * hold the superblock info mutex! */

extern struct shall_fsinfo * shall_fs_list;
extern struct mutex shall_fs_mutex;

#endif /* _SHALL_SUPER_H_ */
