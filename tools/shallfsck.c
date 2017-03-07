/* check/repair an unmounted shallfs device */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <unistd.h>
#include <endian.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include "shallfs-common.h"
#include <shallfs/operation.h>
#include <shallfs/device.h>
#include <shallfs/opdata.h>

enum {
    err_ok               =  0,   /* no errors */
    err_corrected        =  1,   /* errors were corrected */
    err_need_reboot      =  2,   /* errors were corrected, reboot needed */
    err_uncorrected      =  4,   /* errors were left uncorrected */
    err_operation        =  8,   /* operational error */
    err_syntax           = 16,   /* replace user */
    err_cancelled        = 32,   /* cancelled by user */
};

static long autofsck = 0, num_superblocks = 0, progress = -1, force = 0;
static long use_super = 0, readonly = 0, yes_please = 0, do_help = 0;
static int end_progress = 1, last_progress = -1, progress_len = 0;
static int extra_sb_scan = 0;
static const char * device = NULL;

static const shall_options_t options[] = {
    { 'a', &autofsck,        NULL,
      "Same as \"-p\" for compatibility with fsck" },
    { 'b', &num_superblocks, "N-SUPER",
      "Number of superblocks to search if the first one is invalid" },
    { 'C', &progress,        "FILENO",
      "Produces progress information on a file descriptor" },
    { 'f', &force,           NULL,
      "Force consistency check even if the device looks OK" },
    { 'h', &do_help,         NULL,
      "Print this helpful message" },
    { 'l', &use_super,       "N-SUPER",
      "Use the specified superblock instead of searching for one" },
    { 'n', &readonly,        NULL,
      "Do not make any changes, just check and report" },
    { 'p', &autofsck,        NULL,
      "Automatically repair simple problems, suitable for running at boot" },
    { 'y', &yes_please,      NULL,
      "Answer \"yes\" to all questions." },
    {  0,  NULL,             NULL, NULL }
};

static const shall_args_t args[] = {
    { &device,  "DEVICE",  1,
      "The block device (or filename with -c) to initialise" },
    { NULL,     NULL,    0, NULL }
};

static const char * parse_options(int argc, char *argv[]) {
    const char * err = shall_parse_options(argc, argv, options, args);
    if (err) return err;
    if (do_help) return NULL;
    if (autofsck && force)
	return "Cannot use -a/-p with -f";
    if (autofsck && readonly)
	return "Cannot use -a/-p with -n";
    if (autofsck && yes_please)
	return "Cannot use -a/-p with -y";
    if (readonly && yes_please)
	return "Cannot use -n with -y";
    if (num_superblocks != 0 && num_superblocks < 8)
    	return "Invalid number of superblocks, must be at least 8";
    if (use_super > 0 && num_superblocks > 0 && use_super >= num_superblocks)
	return "Cannot give a value for -l greater than the one for -b";
    return NULL;
}

static int sb_same(const shall_sb_data_t * a, const shall_sb_data_t * b) {
    // XXX compare data between a and b
    return 1;
}

static void clear_progress(void) {
    if (progress != 0 || progress_len < 1) return;
    printf("\r%*c\r", progress_len, ' ');
    progress_len = 0;
}

static void show_progress(int pass, int change) {
    char buf[128];
    if (progress < 0) return;
    last_progress += change;
    if (progress == 0) {
	float percent = 100.0 * last_progress / end_progress;
	int n, dash = lround(percent / 2.0), len;
	snprintf(buf, sizeof(buf), "Pass %d |", pass);
	len = strlen(buf);
	for (n = 0; n < dash && len < sizeof(buf); n++)
	    buf[len++] = '=';
	for (; n < 50 && len < sizeof(buf); n++)
	    buf[len++] = ' ';
	if (len < sizeof(buf))
	    snprintf(buf + len, sizeof(buf) - len, "| %5.1f%%", percent);
	else
	    buf[sizeof(buf) - 1] = 0;
	len = strlen(buf);
	putchar('\r');
	if (len < progress_len)
	    printf("%*c\r", progress_len, ' ');
	printf("%s", buf);
	fflush(stdout);
	progress_len = len;
    } else {
	snprintf(buf, sizeof(buf), "%d %d %d %s\n",
		 pass, last_progress, end_progress, device);
	write(progress, buf, strlen(buf));
    }
}

/* read all superblocks and compare information for consistency,
 * rewrite any which does need rewriting */
static int compare_superblocks(int fd, const shall_sb_data_t * sb) {
    struct shall_devsuper dsb;
    shall_sb_data_t ts;
    int uncorrected[sb->num_superblocks];
    int n, n_corrected = 0, n_uncorrected = 0;
    ts = *sb;
    ts.flags &= ~SHALL_SB_DIRTY;
    ts.flags |= SHALL_SB_VALID;
    shall_init_sb(&dsb, &ts, NULL);
    clear_progress();
    printf("Pass 1: scan superblocks\n");
    for (n = 0; n < sb->num_superblocks; n++) {
	int ok = 1;
	if (n != sb->this_superblock) {
	    if (! shall_read_sb(fd, &ts, n) || ! sb_same(sb, &ts))
		ok = 0;
	}
	if (! ok ||
	    (sb->flags & SHALL_SB_DIRTY) ||
	    ! (sb->flags & SHALL_SB_VALID))
	{
	    if (! readonly && shall_write_sb(fd, &dsb, n))
		n_corrected++;
	    else
		uncorrected[n_uncorrected++] = n;
	}
	show_progress(1, 1);
    }
    clear_progress();
    if (n_uncorrected) {
	if (n_corrected)
	    printf("Pass 1 corrected %d errors but left %d uncorrected\n",
		   n_corrected, n_uncorrected);
	else
	    printf("Pass 1 left %d errors uncorrected\n", n_uncorrected);
	printf("Superblocks left with errors");
	for (n = 0; n < n_uncorrected; n++)
	    printf("%c %d", n ? ',' : ':', uncorrected[n]);
	printf("\n");
	return err_uncorrected;
    } else if (n_corrected) {
	printf("Pass 1 corrected %d errors\n", n_corrected);
	return err_corrected;
    } else {
	return err_ok;
    }
}

static void fix_superblock(shall_sb_data_t * sb, shall_check_t chk, FILE * F) {
    const char * sep = "";
    if (chk & shall_check_flags) {
	sb->flags &= SHALL_SB_VALID|SHALL_SB_UPDATE|SHALL_SB_DIRTY;
	if (F) fprintf(F, "%sflags", sep);
	sep = ", ";
    }
    if (chk & shall_check_novalid) {
	sb->flags |= SHALL_SB_VALID;
	if (F) fprintf(F, "%snovalid", sep);
	sep = ", ";
    }
    if (chk & shall_check_lastsb) {
	sb->num_superblocks = 1;
	while (shall_superblock_location(sb->num_superblocks) < sb->device_size)
	    sb->num_superblocks++;
	if (F) fprintf(F, "%slastsb", sep);
	sep = ", ";
    }
    if (chk & shall_check_dataspace) {
	sb->data_space =
	    sb->device_size - SHALL_DEV_BLOCK * sb->num_superblocks;
	if (F) fprintf(F, "%sdataspace", sep);
	sep = ", ";
    }
    if (chk & shall_check_maxlength) {
	sb->max_length = sb->data_length;
	if (F) fprintf(F, "%smaxlength", sep);
	sep = ", ";
    }
    if (chk & shall_check_alignment) {
	sb->alignment /= 8;
	if (sb->alignment < 1) sb->alignment = 1;
	sb->alignment *= 8;
	if (sb->alignment > SHALL_DEV_BLOCK)
	    sb->alignment = SHALL_DEV_BLOCK;
	if (F) fprintf(F, "%salignment", sep);
	sep = ", ";
    }
}

/* read all superblocks and find the best one; this is called only if
 * automatic recovery did not do that for us */
static void do_extra_sb_scan(int fd, shall_sb_data_t * sb) {
    shall_sb_data_t ts;
    int n;
    shall_check_t changed = shall_check_ok;
    clear_progress();
    printf("Pass 0: extra superblock scan due to errors opening device\n");
    for (n = 0; n < sb->num_superblocks; n++) {
	if (n != sb->this_superblock) {
	    if (shall_read_sb_raw(fd, &ts, n)) {
		shall_check_t check = shall_check_sb(fd, sb, n);
		if (! (check & ~shall_check_fixable)) {
		    if (ts.version > sb->version) {
			changed = check;
			*sb = ts;
		    }
		}
	    }
	}
	show_progress(0, 1);
    }
    clear_progress();
    if (changed) fix_superblock(sb, changed, NULL);
}

static int do_full_scan(const char * pname, int fd, shall_sb_data_t * sb) {
    char buffer[65536];
    shall_sb_data_t csb = *sb;
    off_t pos = 0;
    int err = err_ok, lp = 0;
    clear_progress();
    printf("Pass 2: scan data for validity\n");
    /* do a fake read to calculate pointers */
    shall_read_logs(fd, &csb, buffer, 0, 0);
    while (1) {
	ssize_t nr;
	int tp;
	nr = shall_read_data(fd, &csb, buffer, sizeof(buffer), 0);
	if (nr == 0) break;
	if (nr < 0) goto error;
	// XXX verify checksum, size and data of all logs in buffer
	// XXX if any invalid, try to locate next one and then "blank"
	// XXX it (asking first unless -y) by replacing it with an
	// XXX overflow log of some special type and then rewrite buffer
	pos += nr;
	shall_advance_pointers(&csb, nr);
	tp = pos / sizeof(struct shall_devsuper);
	if (tp > lp) {
	    show_progress(2, tp - lp);
	    lp = tp;
	}
    }
    clear_progress();
    return err;
error:
    fprintf(stderr, "%s: %s: Error reading events: %s\n",
	    pname, device, strerror(errno));
    err |= err_uncorrected;
    return err;
}

static const char * status_name(int err) {
    if (err == 0) return "clean";
    if (err & err_uncorrected) return "has errors";
    return "cleaned";
}

/* use unnecessary force to find a superblock somehow */
static int search_superblock(int fd, shall_sb_data_t * sb, const char * pname) {
    off_t limit = lseek(fd, 0, SEEK_END);
    int n_sb = 0;
    if (limit < 0) return 0; /* nothing we can do! */
    while (1) {
	shall_check_t check;
	int n = n_sb++;
	if (shall_superblock_location(n) >= limit) return 0;
	if (! shall_read_sb_raw(fd, sb, n)) continue;
	check = shall_check_sb(fd, sb, n);
	if (check & ~shall_check_fixable) continue;
	/* errors are fixable, try to fix them */
	printf("%s: %s: Rescued partially valid superblock %d, fixed:\n    ",
	       pname, device, n);
	fix_superblock(sb, check, stdout);
	printf("\n");
	extra_sb_scan = 1;
	return 1;
    }
}

int main(int argc, char *argv[]) {
    shall_sb_data_t sb;
    const char * pname = strrchr(argv[0], '/');
    const char * errmsg = parse_options(argc - 1, argv + 1);
    int err = err_ok, fd, full_scan = ! autofsck;
    if (pname)
	pname++;
    else
	pname = argv[0];
    if (do_help) {
	shall_print_help(stdout, pname, options, args);
	return err_ok;
    }
    if (errmsg) {
	fprintf(stderr, "%s: %s\nUse \"%s -h\" for help\n",
		pname, errmsg, pname);
	return err_syntax;
    }
    /* open the device if possible... this will perform any automatic
     * recovery but won't rewrite the data */
    fd = shall_open_device(device, readonly, &sb);
    if (fd < 0) {
	int afd = open(device, readonly ? O_RDONLY : O_RDWR);
	if (afd >= 0) {
	    if (use_super > 0)
	    	if (shall_read_sb(afd, &sb, use_super))
		    fd = afd;
	    if (fd < 0 && full_scan)
		if (search_superblock(afd, &sb, pname))
		    fd = afd;
	    if (fd < 0) {
		errno = EINVAL;
		close(afd);
	    }
	}
	if (fd < 0) {
	    err |= err_uncorrected;
	    goto failed;
	}
    }
    /* if it's in the middle of a resize, ask them to finish it */
    if (sb.flags & SHALL_SB_UPDATE) {
	fprintf(stderr,
		"%s: %s: an update was interrupted, please complete it\n",
		pname, device);
	err |= err_operation;
	if (sb.this_superblock != 0 || (sb.flags & SHALL_SB_DIRTY))
	    err |= err_uncorrected;
	return err;
    }
    /* if superblock 0 was valid and clean, and they didn't say "-f",
     * we have nothing to do */
    if (sb.this_superblock == 0 &&
	! (sb.flags & SHALL_SB_DIRTY) &&
	! force &&
	! extra_sb_scan)
	    goto succeeded;
    end_progress = sb.num_superblocks;
    if (extra_sb_scan) {
	end_progress += sb.num_superblocks;
	do_extra_sb_scan(fd, &sb);
    }
    if (full_scan && ! (err & err_uncorrected))
	end_progress += (sb.data_length + sizeof(struct shall_devsuper) - 1)
		      / sizeof(struct shall_devsuper);
    /* now read all superblocks and compare them for consistency */
    err |= compare_superblocks(fd, &sb);
    if (full_scan && ! (err & err_uncorrected)) {
	do_full_scan(pname, fd, &sb);
    } else if (err && err_uncorrected) {
	clear_progress();
	printf("Skipping pass 2 because of previous uncorrected errors\n");
    } else {
	clear_progress();
	printf("Skipping pass 2 in auto (-a/-p) mode\n");
    }
succeeded:
    /* print report */
    clear_progress();
    printf("%s: %s %s, %lld/%lld (%.1f%%) bytes used\n",
	   pname, device, status_name(err),
	   (long long)sb.data_length, (long long)sb.data_space,
	   100.0 * sb.data_length / sb.data_space);
    return err;
failed:
    clear_progress();
    fprintf(stderr, "%s: %s: %s\n", pname, device, strerror(errno));
    return err;
}


