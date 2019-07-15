/* prepare a device to use as journal for shallfs
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

#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <unistd.h>
#include <endian.h>
#include <stdint.h>
#include <string.h>
#include "shallfs-common.h"

static long force = 0, readonly = 0, quiet = 0, do_help = 0;
static long alignment = 8, num_superblocks = 0, create_it = 0;
static const char * device = NULL, * fs_size = NULL;

static const shall_options_t options[] = {
    { 'a', &alignment,       "ALIGN",
      "Alignment of event logs within the device, multiple of 8 and >= 9" },
    { 'b', &num_superblocks, "N_SUPER",
      "Total number of superblocks, >= 8 and they must fit in the device " },
    { 'c', &create_it,       NULL,
      "Create a regular file suitable for using with mount -oloop" },
    { 'f', &force,           NULL,
      "Skip some sanity checks before proceeding" },
    { 'h', &do_help,         NULL,
      "Print this helpful message" },
    { 'n', &readonly,        NULL,
      "Just show what would be done, do not write anything" },
    { 'q', &quiet,           NULL,
      "Silence some messages describing what the program is doing" },
    {  0,  NULL,             NULL, NULL }
};

static const shall_args_t args[] = {
    { &device,  "DEVICE",  1,
      "The block device (or filename with -c) to initialise" },
    { &fs_size, "SIZE",    0,
      "The size of the device, required with -c, optional otherwise" },
    { NULL,     NULL,    0, NULL }
};

static const char * parse_options(int argc, char *argv[]) {
    const char * err = shall_parse_options(argc, argv, options, args);
    if (err) return err;
    if (do_help) return NULL;
#define mkstr(S, B) S #B
    if (alignment < 1 || alignment > SHALL_DEV_BLOCK || alignment % 8)
	return mkstr("Invalid alignment, must be positive, "
		     "multiple of 8 and <= ", SHALL_DEV_BLOCK);
#undef mkstr
    if (num_superblocks != 0 && num_superblocks < 8)
    	return "Invalid number of superblocks, must be at least 8";
    if (readonly && quiet)
	return "Cannot have both -n and -q";
    if (readonly && create_it)
	return "Cannot have both -n and -c";
    if (create_it && ! fs_size)
	return "Must specify a size when asking to create an image";
    return NULL;
}

int main(int argc, char *argv[]) {
    const char * pname = strrchr(argv[0], '/');
    const char * errmsg = parse_options(argc - 1, argv + 1);
    off_t dev_size;
    int fd, sve;
    if (pname)
	pname++;
    else
	pname = argv[0];
    if (do_help) {
	shall_print_help(stdout, pname, options, args);
	return 0;
    }
    if (errmsg) {
	fprintf(stderr, "%s: %s\nUse \"%s -h\" for help\n",
		pname, errmsg, pname);
	return 1;
    }
    fd = open(device, readonly ? O_RDONLY
			       : create_it ? O_WRONLY|O_CREAT|O_EXCL :
					   O_WRONLY, 0600);
    if (fd < 0) goto out_error;
    if (! force && ! create_it) {
	struct stat sbuff;
	if (fstat(fd, &sbuff) < 0) goto out_close;
	// XXX some OSs require using a character device here
	if (! force && ! S_ISBLK(sbuff.st_mode)) {
	    fprintf(stderr, "%s: %s: not a block device\n", pname, device);
	    close(fd);
	    return 1;
	}
    }
    /* cannot use st_size from the above fstat() becuase it's always 0 */
    dev_size = lseek(fd, 0, SEEK_END);
    if (dev_size < 0) goto out_close;
    if (dev_size % SHALL_DEV_BLOCK)
	dev_size -= dev_size % SHALL_DEV_BLOCK;
    if (fs_size) {
	char * ep;
	off_t use_size = shall_strtol(fs_size, &ep);
	if (*ep || ep == fs_size ||
	    use_size < 16 * SHALL_DEV_BLOCK ||
	    use_size % SHALL_DEV_BLOCK ||
	    (use_size > dev_size && ! create_it))
	{
	    fprintf(stderr, "%s: %s: invalid device size %s\n",
		    pname, device, fs_size);
	    close(fd);
	    if (create_it && ! readonly) unlink(device);
	    return 1;
	}
	if (create_it) {
	    /* in theory, ftruncate can extend the file... but that is
	     * not portable; so we use both a write and ftruncate */
	    if (lseek(fd, use_size - 1, SEEK_SET) < 0) goto out_close;
	    if (write(fd, "\0", 1) < 0) goto out_close;
	    if (ftruncate(fd, use_size) < 0) goto out_close;
	}
	dev_size = use_size;
    }
    if (num_superblocks == 0) {
    	while (shall_superblock_location(num_superblocks) < dev_size)
	    num_superblocks++;
	if (num_superblocks < 8) {
	    fprintf(stderr, "%s: %s: device too small\n", pname, device);
	    if (create_it && ! readonly) unlink(device);
	    close(fd);
	    return 1;
	}
    } else if (shall_superblock_location(num_superblocks) >= dev_size) {
	    fprintf(stderr,
		    "%s: %s: some superblocks are past end of device\n",
		    pname, device);
	    close(fd);
	    if (create_it && ! readonly) unlink(device);
	    return 1;
    }
    if (! quiet) {
	printf("%s: %s: formatting with: -b %ld -a %ld\n",
	       pname, device, num_superblocks, alignment);
	printf("%s: %s: device size is  %lld bytes\n",
	       pname, device, (long long)dev_size);
	printf("%s: %s: journal size is %lld bytes\n",
	       pname, device,
	       (long long)dev_size - num_superblocks * SHALL_DEV_BLOCK);
	if (! readonly)
	    printf("\n%s: %s: Writing superblocks: ", pname, device);
    }
    if (! readonly) {
	struct shall_devsuper ssb;
	shall_sb_data_t data;
	memset(&data, 0, sizeof(data));
	data.num_superblocks = num_superblocks;
	data.device_size = dev_size;
	data.alignment = alignment;
	data.data_start = 0;
	data.data_length = 0;
	data.max_length = 0;
	data.version = 0;
	data.flags = SHALL_SB_VALID;
	data.data_space = dev_size - num_superblocks * SHALL_DEV_BLOCK;
	shall_init_sb(&ssb, &data, NULL);
	if (! shall_write_all_sb(fd, &ssb, ! quiet)) goto out_close;
	if (! quiet) printf(" done\n");
    }
    if (close(fd) < 0) goto out_error;
    if (! quiet && ! readonly)
	printf("%s: %s: device set up successfully\n", pname, device);
    return 0;
out_close:
    sve = errno;
    close(fd);
    errno = sve;
out_error:
    fprintf(stderr, "%s: %s: %s\n", pname, device, strerror(errno));
    if (create_it && ! readonly) unlink(device);
    return 1;
}

