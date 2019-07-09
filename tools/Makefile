# SPDX-License-Identifier: GPL-2.0-only
#
# this file is part of SHALLFS
#
# Copyright (c) 2017-2019 Claudio Calvelli <shallfs@gladserv.com>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, version 2 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program (see the file COPYING in the distribution).
# If not, see <http://www.gnu.org/licenses/>.


all : mkshallfs readshallfs shallfsck shalluserlog

PREFIX = /usr/local

CFLAGS += -D_FILE_OFFSET_BITS=64 -I../include -I../shallfs -Wall -O2

mkshallfs : mkshallfs.o shallfs-common.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o mkshallfs mkshallfs.o shallfs-common.o

mkshallfs.o : mkshallfs.c shallfs-common.h
	$(CC) $(CFLAGS) -c -o mkshallfs.o mkshallfs.c

readshallfs : readshallfs.o shallfs-common.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o readshallfs readshallfs.o shallfs-common.o

readshallfs.o : readshallfs.c shallfs-common.h
	$(CC) $(CFLAGS) -c -o readshallfs.o readshallfs.c

shallfsck : shallfsck.o shallfs-common.o
	$(CC) $(CFLAGS) $(LDFLAGS) -lm -o shallfsck shallfsck.o shallfs-common.o

shallfsck.o : shallfsck.c shallfs-common.h
	$(CC) $(CFLAGS) -c -o shallfsck.o shallfsck.c

shalluserlog : shalluserlog.o shallfs-common.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o shalluserlog shalluserlog.o shallfs-common.o

shalluserlog.o : shalluserlog.c shallfs-common.h
	$(CC) $(CFLAGS) -c -o shalluserlog.o shalluserlog.c

shallfs-common.o : shallfs-common.c shallfs-common.h
	$(CC) $(CFLAGS) -c -o shallfs-common.o shallfs-common.c

install :
	install -d $(PREFIX)/sbin
	install mkshallfs readshallfs shallfsck shalluserlog $(PREFIX)/sbin

clean :
	rm -f *.o mkshallfs readshallfs shallfsck shalluserlog

