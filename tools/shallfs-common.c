/* common functions to update a shallfs device; used by mkshallfs,
 * shallfsck, readshallfs and tuneshallfs */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stddef.h> /* for offsetof */
#include <memory.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <sys/sysmacros.h>
#include "shallfs-common.h"

#define PROCMOUNTS "/proc/fs/shallfs/mounted"
#define PROCDIR    "/proc/fs/shallfs/%x:%x"
#define PROCINFO   "info"
#define PROCLOGS   "blog"
#define PROCCTRL   "ctrl"

typedef enum {
    proc_control,
    proc_blocking,
    proc_nonblocking,
    proc_mode_MAX
} proc_mode_t;

static int proc_mode[proc_mode_MAX] = {
    [proc_control]     = O_WRONLY,
    [proc_blocking]    = O_RDONLY | O_NONBLOCK,
    [proc_nonblocking] = O_RDONLY,
};

typedef struct {
    char name;
    long multiply;
} unit_t;

static const unit_t units[] = {
    { 'k',            1024 },
    { 'b', SHALL_DEV_BLOCK },
    { 'm',         1048576 },
    { 'g',      1073741824 },
    {  0,                0 }
};

/* like strtol but always uses default base and accepts a number with unit */
long shall_strtol(const char * num, char ** end) {
    long res = strtol(num, end, 0);
    int u;
    if (num == *end || ! **end) return res;
    for (u = 0; units[u].name && units[u].name != **end; u++)
	;
    if (! units[u].name) return res;
    (*end)++;
    return res * units[u].multiply;
}

/* parse command line and return error message, NULL if OK */
const char * shall_parse_options(int argc, char *argv[],
				 const shall_options_t * options,
				 const shall_args_t * args)
{
    static char errmsg[80];
    int nargs = 0;
    while (argc > 0) {
	char *a = *argv++;
	argc--;
	if (*a == '-') {
	    while (*a == '-') a++;
	    while (*a) {
		char opt = *a++;
		int n;
		for (n = 0; options[n].name && options[n].name != opt; n++)
		    ;
		if (! options[n].name) {
		    snprintf(errmsg, sizeof(errmsg), "Unknown option -%c", opt);
		    return errmsg;
		}
		if (options[n].valname) {
		    char * ep;
		    if (! *a) {
			if (argc < 1) {
			    snprintf(errmsg, sizeof(errmsg),
				     "Missing %s for -%c",
				     options[n].valname, opt);
			    return errmsg;
			}
			a = *argv++;
			argc--;
		    }
		    *options[n].value = shall_strtol(a, &ep);
		    if (a == ep) {
			snprintf(errmsg, sizeof(errmsg),
				 "Invalid %s (%s) for -%c",
				 options[n].valname, a, opt);
			return errmsg;
		    }
		    a = ep;
		} else {
		    *options[n].value = 1;
		}
	    }
	} else if (! args || ! args[nargs].value) {
	    return "Too many command line arguments";
	} else {
	    *args[nargs++].value = a;
	}
    }
    if (args && args[nargs].value && args[nargs].required) {
	snprintf(errmsg, sizeof(errmsg),
		 "Please provide %s", args[nargs].valname);
	return errmsg;
    }
    return NULL;
}

/* print help text */
void shall_print_help(FILE * F, const char * pname,
		      const shall_options_t * options,
		      const shall_args_t * args)
{
    int n, sq;
    fprintf(F, "Usage: %s", pname);
    for (n = sq = 0; options[n].name; n++) {
	if (options[n].valname) continue;
	if (! sq) {
	    sq = 1;
	    fprintf(F, " [-");
	}
	putc(options[n].name, F);
    }
    if (sq) fprintf(F, "]");
    for (n = 0; options[n].name; n++) {
	if (! options[n].valname) continue;
	fprintf(F, " [-%c %s]", options[n].name, options[n].valname);
    }
    for (n = sq = 0; args[n].value; n++) {
	putc(' ', F);
	if (! args[n].required) {
	    sq++;
	    putc('[', F);
	}
	fprintf(F, "%s", args[n].valname ? args[n].valname : "NAME");
    }
    while (sq-- > 0)
	putc(']', F);
    putc('\n', F);
    for (n = 0; options[n].name; n++) {
	if (! options[n].descr) continue;
	fprintf(F, "-%c ", options[n].name);
	if (options[n].valname)
	    fprintf(F, "%s\n    ", options[n].valname);
	else
	    putc(' ', F);
	fprintf(F, "%s\n", options[n].descr);
    }
    for (n = 0; args[n].value; n++) {
	int len;
	if (! args[n].descr) continue;
	fprintf(F, "%s", args[n].valname);
	len = strlen(args[n].valname);
	if (len < 4) {
	    fprintf(F, "%.*s", 4 - len, "");
	} else {
	    fprintf(F, "\n    ");
	}
	fprintf(F, "%s\n", args[n].descr);
    }
}

/* initialise a new superblock structure */
void shall_init_sb(struct shall_devsuper * ssb, const shall_sb_data_t * data,
		   const shall_sb_info_t * change)
{
    memset(ssb, 0, sizeof(*ssb));
    strncpy(ssb->magic1, SHALL_SB_MAGIC, sizeof(ssb->magic1));
    ssb->device_size = htole64(data->device_size);
    ssb->data_space = htole64(data->data_space);
    ssb->data_start = htole64(data->data_start);
    ssb->data_length = htole64(data->data_length);
    ssb->max_length = htole64(data->max_length);
    ssb->version = htole64(data->version);
    ssb->flags = htole32(data->flags);
    ssb->alignment = htole32(data->alignment);
    ssb->num_superblocks = htole32(data->num_superblocks);
    ssb->new_size = htole64(change ? change->dev_size : 0);
    ssb->new_alignment = htole32(change ? change->alignment : 0);
    ssb->new_superblocks = htole32(change ? change->num_superblocks : 0);
    strncpy(ssb->magic2, SHALL_SB_MAGIC, sizeof(ssb->magic2));
}

/* calculate generic crc32; this is not the optimised function one finds
 * in the kernel, but it is equivalent */
static unsigned int crc32(unsigned int start, const void * _data, size_t len) {
    const unsigned char * data = _data;
    while (len-- > 0) {
	int i;
	start ^= *data++;
	for (i = 0; i < 8; i++)
	    start = (start >> 1) ^ ((start & 1) ? 0xedb88320 : 0);
    }
    return start;
}

/* calculate checksum for superblock structure */
unsigned int shall_checksum_sb(const struct shall_devsuper * sh) {
    return crc32(0x4c414853, sh, shall_superblock_checksize);
}

/* calculate checksum for log header structure */
unsigned int shall_checksum_log(const struct shall_devheader * dh) {
    return crc32(0x4c414853, dh, shall_devheader_checksize);
}

/* read some data */
static int read_data(int fd, void * _buf, size_t len) {
    char * buf = _buf;
    while (len > 0) {
	ssize_t nr = read(fd, buf, len);
	if (nr < 0) return 0;
	if (nr == 0) {
	    errno = EINVAL; /* anybody has a better idea? */
	    return 0;
	}
	buf += nr;
	len -= nr;
    }
    return 1;
}

/* write some data */
static int write_data(int fd, const void * _buf, size_t len) {
    const char * buf = _buf;
    while (len > 0) {
	ssize_t nw = write(fd, buf, len);
	if (nw < 0) return 0;
	if (nw == 0) {
	    errno = ENOSPC; /* anybody has a better idea? */
	    return 0;
	}
	buf += nw;
	len -= nw;
    }
    return 1;
}

/* read a superblock from disk, check checksum and decode information */
static int read_sb(int fd, shall_sb_data_t * sb, int which) {
    struct shall_devsuper ssb;
    if (lseek(fd, shall_superblock_location(which), SEEK_SET) < 0) return 0;
    if (! read_data(fd, &ssb, sizeof(ssb))) return 0;
    /* check: checksum is valid */
    if (le32toh(ssb.checksum) != shall_checksum_sb(&ssb)) {
	errno = EINVAL;
	return 0;
    }
    /* check: both magic strings present */
    if (strncmp(ssb.magic1, SHALL_SB_MAGIC, sizeof(ssb.magic1)) != 0)
	goto invalid;
    if (strncmp(ssb.magic2, SHALL_SB_MAGIC, sizeof(ssb.magic2)) != 0)
	goto invalid;
    /* decode all data */
    sb->version = le64toh(ssb.version);
    sb->device_size = le64toh(ssb.device_size);
    sb->data_space = le64toh(ssb.data_space);
    sb->data_start = le64toh(ssb.data_start);
    sb->data_length = le64toh(ssb.data_length);
    sb->max_length = le64toh(ssb.max_length);
    sb->real_start = -1;
    sb->flags = le32toh(ssb.flags);
    sb->num_superblocks = le32toh(ssb.num_superblocks);
    sb->this_superblock = le32toh(ssb.this_superblock);
    sb->alignment = le32toh(ssb.alignment);
    sb->next_superblock = -1;
    return 1;
invalid:
    errno = EINVAL;
    return 0;
}

/* perform consistency checks on superblock */
shall_check_t shall_check_sb(int fd, const shall_sb_data_t * sb, int which) {
    off_t eod = lseek(fd, 0, SEEK_END), dspace;
    shall_check_t result = shall_check_ok;
    if (eod < 0) result |= shall_check_ioerr;
    /* check: flags contains SHALL_SB_VALID */
    if (! (sb->flags & SHALL_SB_VALID)) result |= shall_check_novalid;
    if (sb->flags & ~(SHALL_SB_VALID|SHALL_SB_DIRTY|SHALL_SB_UPDATE))
	result |= shall_check_flags;
    /* check: device_size is <= the physical size of the device */
    if (eod > 0 && sb->device_size > eod) result |= shall_check_toobig;
    /* check: device_size is a multiple of SHALL_DEV_BLOCK and >= 65536 */
    if (sb->device_size % SHALL_DEV_BLOCK) result |= shall_check_nonblock;
    if (sb->device_size < 65536) result |= shall_check_toosmall;
    /* check: num_superblocks > 8 */
    if (sb->num_superblocks <= 8) result |= shall_check_toosmall;
    /* check: data_space + SHALL_DEV_BLOCK * num_superblocks == device_size */
    dspace = sb->device_size - SHALL_DEV_BLOCK * sb->num_superblocks;
    if (sb->data_space != dspace) result |= shall_check_dataspace;
    /* check: 0 <= data_start < data_space */
    if (sb->data_start < 0) result |= shall_check_datastart;
    if (sb->data_start >= dspace) result |= shall_check_datastart;
    /* check: 0 <= data_length <= data_space */
    if (sb->data_length < 0) result |= shall_check_datalength;
    if (sb->data_length > dspace) result |= shall_check_datalength;
    /* check: data_length <= max_length <= data_space */
    if (sb->max_length < sb->data_length) result |= shall_check_maxlength;
    if (sb->max_length > dspace) result |= shall_check_maxlength;
    /* check: alignment is a multiple of 8 and >= 8 */
    if (sb->alignment % 8) result |= shall_check_alignment;
    if (sb->alignment < 8) result |= shall_check_alignment;
    /* check: location(last superblock) + sizeof(sb) <= device_size */
    if (shall_superblock_location(sb->num_superblocks - 1) +
	sizeof(struct shall_devsuper) > sb->device_size)
	    result |= shall_check_lastsb;
    return result;
}

/* read a superblock from disk */
int shall_read_sb(int fd, shall_sb_data_t * sb, int which) {
    if (! read_sb(fd, sb, which)) return 0;
    return shall_check_sb(fd, sb, which) == shall_check_ok;
}

/* like shall_read_sb but without the consistency checks */
int shall_read_sb_raw(int fd, shall_sb_data_t * sb, int which) {
    return read_sb(fd, sb, which);
}

/* write a superblock to disk; the structure must have been prepared by
 * one of the other functions; return 0 on error, 1 OK */
int shall_write_sb(int fd, struct shall_devsuper * ssb, int which) {
    if (lseek(fd, shall_superblock_location(which), SEEK_SET) < 0) return 0;
    ssb->this_superblock = htole32(which);
    ssb->checksum = htole32(shall_checksum_sb(ssb));
    return write_data(fd, ssb, sizeof(*ssb));
}

/* write all superblocks */
int shall_write_all_sb(int fd, struct shall_devsuper * ssb, int verbose) {
    int which, last = le32toh(ssb->num_superblocks), backspace = 0;
    for (which = 0; which < last; which++) {
	if (! shall_write_sb(fd, ssb, which)) return 0;
	if (verbose) {
	    char dbuff[32];
	    snprintf(dbuff, sizeof(dbuff), "%d/%d", which + 1, last);
	    while (backspace-- > 0) putc('\b', stdout);
	    printf("%s", dbuff);
	    backspace = strlen(dbuff);
	    if (which % 16) fflush(stdout);
	}
    }
    return 1;
}

/* find a working superblock */
static int search_superblock(int fd, shall_sb_data_t * sb) {
    off_t limit = lseek(fd, 0, SEEK_END);
    int n = 0;
    if (limit < 0) return 0;
    while (1) {
	n++;
	if (shall_superblock_location(n) >= limit) {
	    errno = EINVAL;
	    return 0;
	}
	if (shall_read_sb(fd, sb, n))
	    return 1;
    }
}

/* read all superblocks and find the best one */
static void scan_all_superblocks(int fd, shall_sb_data_t * sb) {
    shall_sb_data_t temp;
    int ns = sb->num_superblocks, n;
    for (n = 0; n < ns; n++) {
	if (shall_read_sb(fd, &temp, n))
	    if (temp.version > sb->version)
		*sb = temp;
    }
}

/* find mounted device by underlying fspath or mountpoint */
int shall_find_device(const char * path, dev_t * dev) {
    char buffer[4096];
    struct stat sbuff;
    FILE * F;
    int rlen, rmaj, rmin;
    if (stat(path, &sbuff) < 0) return 0;
    rmaj = major(sbuff.st_dev);
    rmin = minor(sbuff.st_dev);
    F = fopen(PROCMOUNTS, "r");
    if (! F) return 0;
    rlen = strlen(path);
    while (rlen > 0 && path[rlen - 1] == '/') rlen--;
    if (rlen == 0 && *path) rlen = 1;
    while (fgets(buffer, sizeof(buffer), F)) {
	const char * src;
	int maj, min, flen, bptr, eptr, ok;
	if (sscanf(buffer, "%x:%x %d %n%*s%n",
		   &maj, &min, &flen, &bptr, &eptr) < 3)
	    break;
	if (maj == rmaj && min == rmin) goto found;
	if (flen != rlen) continue;
	ok = 1;
	src = path;
	while (bptr < eptr && ok) {
	    char c = buffer[bptr++], d = *src++;
	    if (c == '%') {
		char hex[5] = "0x00";
		if (bptr + 2 >= eptr) {
		    ok = 0;
		    break;
		}
		hex[2] = buffer[bptr++];
		hex[3] = buffer[bptr++];
		if (! isxdigit((int)(unsigned char)hex[2]))
		    ok = 0;
		else if (! isxdigit((int)(unsigned char)hex[3]))
		    ok = 0;
		else if (strtol(hex, NULL, 0) != d)
		    ok = 0;
	    } else {
	    	if (c != d) ok = 0;
	    }
	}
	if (! ok) continue;
    found:
	*dev = makedev(maj, min);
	fclose(F);
	return 1;
    }
    fclose(F);
    return 0;
}

/* open a device and perform any automatic recovery */
int shall_open_device(const char * dev, int ro, shall_sb_data_t * sb) {
    int fd = open(dev, ro ? O_RDONLY : O_RDWR);
    if (fd < 0) return fd;
    /* read first superblock */
    if (! shall_read_sb(fd, sb, 0)) {
	/* failed, look for an alternate superblock */
	if (! search_superblock(fd, sb)) {
	    int sve = errno;
	    close(fd);
	    errno = sve;
	    return -1;
	}
    }
    if (sb->flags & SHALL_SB_UPDATE) {
	close(fd);
	errno = EBUSY;
	return -1;
    }
    if (sb->flags & SHALL_SB_DIRTY)
	scan_all_superblocks(fd, sb);
    return fd;
}

/* read data from disk; return amount of buffer used, 0 if EOF, negative
 * if error; does not update superblock information, and the last event
 * may be truncated depending on space in the buffer; caller must make
 * sure real_start and next_superblock are correct as this cannot update
 * the superblock to recalculate them */
ssize_t shall_read_data(int fd, const shall_sb_data_t * sb,
			char * dest, size_t len, int verbose)
{
    off_t rs = sb->real_start, data = sb->data_length, start = sb->data_start;
    ssize_t done = 0;
    int next = sb->next_superblock;
    while (len > 0 && data > 0) {
	/* read until the next superblock, or until the buffer has been filled,
	 * or until we run out of data, whichever is smaller */
	off_t ns = next < sb->num_superblocks
		 ? (shall_superblock_location(next) - SHALL_SB_OFFSET)
		 : sb->device_size;
	size_t todo = ns - sb->real_start;
	ssize_t nr;
	if (todo > len - done) todo = len - done;
	if (todo > data) todo = data;
	if (verbose)
	    printf("read_logs @%lld (sb=%d %lld) %ld [%lld]\n",
		   (long long)rs, next, (long long)ns,
		   (long)todo, (long long)rs + todo);
	if (lseek(fd, rs, SEEK_SET) < 0) return -1;
	nr = read(fd, dest + done, todo);
	if (nr < 0) return -1;
	if (nr == 0) break;
	done += nr;
	rs += nr;
	data -= nr;
	start += nr;
	if (rs < ns) continue;
	next++;
	rs += SHALL_DEV_BLOCK;
	if (rs < sb->device_size) continue;
	next = 1;
	rs = SHALL_DEV_BLOCK;
    }
    return done;
}

/* advance superblock pointers by given offset */
void shall_advance_pointers(shall_sb_data_t * sb, size_t len) {
    off_t rs = sb->real_start, data = sb->data_length, start = sb->data_start;
    int next = sb->next_superblock;
    while (len > 0) {
	off_t ns = next < sb->num_superblocks
		 ? (shall_superblock_location(next) - SHALL_SB_OFFSET)
		 : sb->device_size;
	size_t todo = ns - sb->real_start;
	if (todo > len) todo = len;
	if (todo > data) todo = data;
	len -= todo;
	rs += todo;
	data -= todo;
	start += todo;
	if (rs < ns) continue;
	next++;
	rs += SHALL_DEV_BLOCK;
	if (rs < sb->device_size) continue;
	next = 1;
	rs = SHALL_DEV_BLOCK;
    }
    sb->data_length = data;
    sb->real_start = rs;
    sb->next_superblock = next;
    sb->data_start = start;
}

/* read events from disk; return amount of buffer used, 0 if EOF, negative
 * if error; if successful, updates superblock information */
ssize_t shall_read_logs(int fd, shall_sb_data_t * sb,
			char * dest, size_t len, int verbose)
{
    ssize_t done;
    int next = sb->next_superblock;
    /* calculate disk block and offset corresponding to sb->data_start, but
     * only if it hadn't been calculated before */
    if (next < 0) {
	off_t rs = sb->data_start;
	next = 0;
	while (next < sb->num_superblocks &&
	       shall_superblock_location(next) - SHALL_SB_OFFSET <= rs)
	{
	    next++;
	    rs += SHALL_DEV_BLOCK;
	}
	sb->real_start = rs;
	sb->next_superblock = next;
    }
    done = shall_read_data(fd, sb, dest, len, verbose);
    if (done <= 0) return done;
    /* OK, we've read as much data as there was, or as much it fits in the
     * buffer; the last event will be truncated so adjust things */
    len = 0;
    while (len < done) {
	struct shall_devheader lh;
	unsigned int chk;
	int nh;
	if (done - len < sizeof(lh)) break;
	memcpy(&lh, dest + len, sizeof(lh));
	chk = shall_checksum_log(&lh);
	if (chk != le32toh(lh.checksum)) {
	    if (len == 0) {
		errno = EINVAL;
		return -1;
	    }
	    break;
	}
	nh = le32toh(lh.next_header);
	if (nh < sizeof(lh)) {
	    if (len == 0) {
		errno = EINVAL;
		return -1;
	    }
	    break;
	}
	if (done - len < nh) break;
	len += nh;
    }
    if (verbose)
	printf("read_logs done=%ld -> %ld\n", (long)done, (long)len);
    if (len == 0) return 0;
    /* and now advance pointers to the location we've just calculated */
    shall_advance_pointers(sb, len);
    return len;
}

/* open a file in /proc/fs/shallfs/DEVICE */
static int open_proc(dev_t dev, const char * name, proc_mode_t mode) {
    char procfile[sizeof(PROCDIR) + strlen(name) + 32];
    if (mode >= proc_mode_MAX || (int)mode < 0) {
	errno = -EINVAL;
	return -1;
    }
    snprintf(procfile, sizeof(procfile), PROCDIR "/%s",
	     major(dev), minor(dev), name);
    return open(procfile, proc_mode[mode]);
}

static int find_kw(const char * data, int datalen,
		   const char * keyword, int keylen, long long * val)
{
    if (datalen <= keylen) return 0;
    if (data[keylen] != ':') return 0;
    if (strncmp(data, keyword, keylen) != 0) return 0;
    if (sscanf(data + keylen + 1, "%lld", val) < 1) return 0;
    return 1;
}

/* read superblock information from a mounted filesystem */
int shall_mounted_info(dev_t dev, shall_sb_data_t * sb) {
    char buffer[4097];
    ssize_t nr;
    int fd = open_proc(dev, PROCINFO, proc_blocking), ptr;
    if (fd < 0) return 0;
    nr = read(fd, buffer, sizeof(buffer));
    if (nr <= 0) {
	int sv = nr < 0 ? errno : EINVAL;
	close(fd);
	errno = sv;
	return 0;
    }
    buffer[nr] = 0;
    memset(sb, 0, sizeof(*sb));
    sb->real_start = -1;
    sb->next_superblock = -1;
    ptr = 0;
    while (ptr < nr) {
	long long val;
	int bptr = ptr, eptr;
	while (ptr < nr && buffer[ptr] != '\n') ptr++;
	eptr = ptr - bptr;
	while (ptr < nr && buffer[ptr] == '\n') buffer[ptr++] = 0;
#define find(kw, dest) \
	if (find_kw(buffer + bptr, eptr, kw, strlen(kw), &val)) { \
	    sb->dest = val; \
	    continue; \
	}
	find("version",  version);
	find("devsize",  device_size);
	find("space",    data_space);
	find("start",    data_start);
	find("size",     data_length);
	find("maxsize",  max_length);
	find("flags",    flags);
	find("nsuper",   num_superblocks);
	find("align",    alignment);
#undef find
    }
    close(fd);
    return 1;
}

/* open mounted filesystem's logfile */
int shall_open_logfile(dev_t dev, int blocking, int verbose) {
    return open_proc(dev, PROCLOGS, blocking ? proc_blocking : proc_nonblocking);
}

/* send a command to a mounted filesystem */
static int shall_ctrl(dev_t dev, const char * command) {
    int fd = open_proc(dev, PROCCTRL, proc_control);
    if (fd < 0) return 0;
    if (write(fd, command, strlen(command)) < 0) {
	int sverr = errno;
	close(fd);
	errno = sverr;
	return 0;
    }
    if (close(fd) < 0) return 0;
    return 1;
}

int shall_ctrl_commit(dev_t dev) {
    return shall_ctrl(dev, "commit\n");
}

int shall_ctrl_clear(dev_t dev, int discard) {
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "clear %d\n", discard);
    return shall_ctrl(dev, buffer);
}

int shall_ctrl_userlog(dev_t dev, const char * text) {
    char buffer[144];
    snprintf(buffer, sizeof(buffer), "userlog %.128s\n", text);
    return shall_ctrl(dev, buffer);
}

