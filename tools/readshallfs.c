/* read an unmounted device and show its contents */

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
#include <ctype.h>
#include "shallfs-common.h"
#include <shallfs/operation.h>
#include <shallfs/device.h>
#include <shallfs/opdata.h>

/* see print_debug_log() */
typedef struct follow_s follow_t;
typedef enum {
    type_kmalloc = 'k',
    type_vmalloc = 'v',
    type_name = 'n',
    type_any = 0
} memtype_t;
struct follow_s {
    follow_t * next;
    follow_t * prev;
    memtype_t type;
    unsigned long address;
    size_t size;
    int lineno;
    char filename[0];
};

static long help = 0, sbinfo = 0, all_logs = 0, mounted = 0, blocking = 0;
static long append = 0, input = 0, debug_prog = 0, debug_logs = 0;
static long clear_logs = 0, max_logs = 0;
static const char * device = NULL, * filename = NULL;
static follow_t * follow = NULL;

static const shall_options_t options[] = {
    { 'a', &append,          NULL,
      "If file-name is specified, append to it instead of overwriting" },
    { 'c', &clear_logs,      NULL,
      "Clear logs, remove all logs from device" },
    { 'd', &debug_logs,      NULL,
      "Print debug logs; incompatible with -l" },
    { 'D', &debug_prog,      NULL,
      "Print extra debugging information" },
    { 'h', &help,            NULL,
      "Print this helpful message" },
    { 'i', &input,           NULL,
      "Interpret device-name as a file which was produced by this program" },
    { 'l', &all_logs,        NULL,
      "Show all event logs (default if -i)" },
    { 'm', &mounted,         NULL,
      "Search for a mounted filesystem, device-name is mountpoint or fspath" },
    { 'p', &max_logs,        "NUM-LOGS",
       "Show partial logs only, stop after NUM-LOGS events" },
    { 's', &sbinfo,          NULL,
      "Show filesystem information (default if no -l and no -i)" },
    { 'w', &blocking,        NULL,
      "With -m, wait for new events on end of file (default: stop at EOF)" },
    {  0,  NULL,             NULL, NULL }
};

static const shall_args_t args[] = {
    { &device,   "DEVICE",  1,
      "The block device to read, or the mountpoint/fspath with -m" },
    { &filename, "FILE",    0,
      "Output file name, events will be stored here if this is provided" },
    { NULL,      NULL,           0, NULL }
};

/* print a line with file attributes; can fold if it looks too long */
static void print_attr(const char * head1, const char * head2,
		       const char * tail, const struct shall_devattr * da)
{
#define append \
    sl = strlen(buffer); \
    if (len + sl > 80 && len > blen) { \
    	printf("%s%s", tail, head2); \
	len = strlen(head2); \
    } \
    printf("%s", buffer); \
    len += sl
#define print(fmt, args...) { \
    int sl; \
    snprintf(buffer, sizeof(buffer), fmt, args); \
    append; \
}
#define print_time(what) { \
    time_t sec = le64toh(da->what##_sec); \
    int sl, nsec = le32toh(da->what##_nsec); \
    snprintf(buffer, sizeof(buffer) - 25, " " #what "=%lld.%03d", \
    	     (long long)sec, nsec / 1000000); \
    sl = strlen(buffer); \
    strftime(buffer + sl, sizeof(buffer) - sl, \
	     " (%Y-%m-%d %H:%M:%S %Z)", localtime(&sec)); \
    append; \
}
    char buffer[128];
    int flags = le32toh(da->flags), len = strlen(head1), blen = len;
    printf("%s", head1);
    if (flags & shall_attr_mode)
	print(" mode=%04o", le32toh(da->mode));
    if (flags & shall_attr_user)
	print(" uid=%d", le32toh(da->user));
    if (flags & shall_attr_group)
	print(" gid=%d", le32toh(da->group));
    if (flags & (shall_attr_block | shall_attr_char | shall_attr_size)) {
	uint64_t num = le64toh(da->size);
	if (flags & shall_attr_size) {
	    print(" size=%lld", (long long)num);
	} else {
	    print(" %cdev=%x:%x",
		  (flags & shall_attr_block) ? 'b' : 'c',
		  (unsigned int)(num >> 32), (unsigned int)(num & 0xffffffff));
	}
    }
    if (flags & shall_attr_atime)
	print_time(atime);
    if (flags & shall_attr_mtime)
	print_time(mtime);
    printf("%s", tail);
#undef print_time
#undef print
#undef append
}

/* see follow_message() */
static inline unsigned long strntol(const char * num, int size, int base) {
    char ncopy[size + 1];
    strncpy(ncopy, num, size);
    ncopy[size] = 0;
    return strtoul(ncopy, NULL, base);
}

static follow_t * lookup_follow(memtype_t type, unsigned long address) {
    follow_t * fw = follow;
    while (fw) {
	if ((type == type_any || type == fw->type) &&
	    address == fw->address)
		return fw;
	fw = fw->next;
    }
    return NULL;
}

static void remove_follow(follow_t * fw) {
    if (fw->next)
	fw->next->prev = fw->prev;
    if (fw->prev)
	fw->prev->next = fw->next;
    else
	follow = fw->next;
    free(fw);
}

/* "follow" a debug message */
static const char * follow_message(const char * message, int msglen,
				   const char * filename, int fnlen, int line)
{
    static char msgbuff[1024];
    int is_alloc = 0, offset = 0;
    memtype_t type = type_any;
    if (msglen < 1) return "";
    switch (message[0]) {
	case 'k' : case 'v' :
	    type = message[0];
	    if (msglen > 10 && strncmp(message + 1, "malloc(", 7) == 0) {
		is_alloc = 1;
		offset = 8;
	    } else if (strncmp(message + 1, "free(", 5) == 0) {
		is_alloc = 0;
		offset = 6;
	    }
	    break;
	case 'g' :
	    if (msglen > 10 && strncmp(message + 1, "etname(", 7) == 0) {
		type = type_name;
		is_alloc = 1;
		offset = 8;
	    }
	    break;
	case 'p' :
	    if (msglen > 10 && strncmp(message + 1, "utname(", 7) == 0) {
		type = type_name;
		is_alloc = 0;
		offset = 8;
	    }
	    break;
    }
    if (type == type_any) return "";
    if (is_alloc) {
	follow_t * fw;
	unsigned long address;
	size_t size;
	int eq;
	if (message[offset] == '?') {
	    size = 0;
	} else {
	    size = strntol(message + offset, msglen - offset, 0);
	}
	for (eq = offset + 2; eq < msglen && message[eq] != '='; eq++) ;
	eq++;
	if (eq >= msglen) return "";
	address = strntol(message + eq, msglen - eq, 16);
	fw = lookup_follow(type_any, address);
	if (fw) {
	    snprintf(msgbuff, sizeof(msgbuff), " ** duplicate? (%s:%d)",
		     fw->filename, fw->lineno);
	    return msgbuff;
	}
	fw = malloc(sizeof(*fw) + fnlen + 1);
	if (! fw) return " ** cannot allocate memory!";
	fw->prev = NULL;
	fw->next = follow;
	if (follow) follow->prev = fw;
	fw->type = type;
	fw->address = address;
	fw->size = size;
	fw->lineno = line;
	strncpy(fw->filename, filename, fnlen);
	fw->filename[fnlen] = 0;
	follow = fw;
	return "";
    } else {
	follow_t * fw;
	unsigned long address;
	address = strntol(message + offset, msglen - offset, 16);
	fw = lookup_follow(type, address);
	if (fw) {
	    remove_follow(fw);
	    return "";
	}
	fw = lookup_follow(type_any, address);
	if (fw) {
	    snprintf(msgbuff, sizeof(msgbuff),
		     " ** allocated with %cmalloc? (%s:%d)",
		     fw->type, fw->filename, fw->lineno);
	    remove_follow(fw);
	    return msgbuff;
	}
	return "** never allocated or double-freed!";
    }
}

#define getdata(dest) \
	memcpy(&(dest), data, sizeof((dest))); \
	data += sizeof((dest))

/* send a debug event to file */
static int print_debug_log(FILE * F, const char * data) {
    char ts[64];
    struct shall_devheader dh;
    struct shall_devfileid df;
    time_t req;
    const char * message = "", * filename = "", * fmode = "";
    int length, op, flags, msglen = 0, fnlen = 0, line;
    getdata(dh);
    length = le32toh(dh.next_header);
    op = le32toh(dh.operation);
    if (op) return length;
    req = le64toh(dh.req_sec);
    line = le32toh(dh.result);
    flags = le32toh(dh.flags);
    if (flags & SHALL_LOG_FILE1) {
	getdata(df);
	msglen = le32toh(df.fileid);
	message = data;
	data += msglen;
    }
    if (flags & SHALL_LOG_FILE2) {
	getdata(df);
	fnlen = le32toh(df.fileid);
	filename = data;
	data += fnlen;
    }
    fmode = follow_message(message, msglen, filename, fnlen, line);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S %Z", localtime(&req));
    fprintf(F, "%10lld.%03d %s %.*s:%d %.*s%s\n",
	    (long long)req, le32toh(dh.req_nsec) / 1000000, ts,
	    fnlen, filename, line, msglen, message, fmode);
    return length;
}

static inline void print_perm(char sep, char who, int id, int perm) {
    printf("%c%c:", sep, who);
    if (id >= 0) printf("%d", id);
    printf(":%c%c%c",
	   (perm & shall_acl_read) ? 'r' : '-',
	   (perm & shall_acl_write) ? 'w' : '-',
	   (perm & shall_acl_execute) ? 'x' : '-');
    /* the following don't seem to be used by anybody... */
    if ((perm & shall_acl_what) == shall_acl_add)
	putchar('a');
    if ((perm & shall_acl_what) == shall_acl_delete)
	putchar('d');
}

/* print a single event */
static int print_log(off_t where, const char * data, int count) {
    char ts[64];
    struct shall_devheader dh;
    struct shall_devregion dr;
    struct shall_devhash dc;
    struct shall_devfileid df;
    struct shall_devsize ds;
    struct shall_devattr da;
    struct shall_devacl dl;
    struct shall_devacl_entry de;
    struct shall_devxattr dx;
    const char * ba, * name1 = "", * name2 = "";
    time_t req;
    int length, op, len1 = 0, len2 = 0, flags, result, n;
    getdata(dh);
    length = le32toh(dh.next_header);
    req = le64toh(dh.req_sec);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S %Z", localtime(&req));
    printf("  %-7d", count);
    if (debug_prog)
	printf(" @%-10lld len=%-5d", (long long)where, length);
    printf(" %10lld.%03d (%s)\n",
	   (long long)req, le32toh(dh.req_nsec) / 1000000, ts);
    op = le32toh(dh.operation);
    result = le32toh(dh.result);
    if (op != 0) {
	if (op < 0) {
	    op = -op;
	    ba = "before";
	} else {
	    ba = "after ";
	}
	printf("          %s op#%-2d %-16s -> %d\n",
	       ba, op,
	       op > 0 && op < SHALL_MAX_OPCODE ? shall_opdata[op].name : "?",
	       result);
    }
    flags = le32toh(dh.flags);
    if (flags & SHALL_LOG_FILE1) {
	getdata(df);
	len1 = le32toh(df.fileid);
	if (op == 0)
	    name1 = data;
	else
	    printf("          [%.*s]\n", len1, data);
	data += len1;
    }
    if (flags & SHALL_LOG_FILE2) {
	getdata(df);
	len2 = le32toh(df.fileid);
	if (op == 0)
	    name2 = data;
	else
	    printf("          [%.*s]\n", len2, data);
	data += len2;
    }
    switch (flags & SHALL_LOG_DMASK) {
	case SHALL_LOG_ATTR :
	    getdata(da);
	    print_attr("          attr:", "               ", "\n", &da);
	    break;
	case SHALL_LOG_REGION :
	    getdata(dr);
	    printf("          id=%d region=%lld:%lld\n",
		   le32toh(dr.fileid), (long long)le64toh(dr.start),
		   (long long)le64toh(dr.length));
	    break;
	case SHALL_LOG_FILEID :
	    getdata(df);
	    printf("          id=%d\n", le32toh(df.fileid));
	    break;
	case SHALL_LOG_SIZE :
	    getdata(ds);
	    printf("          size=%lld\n", (long long)le64toh(ds.size));
	    break;
	case SHALL_LOG_ACL :
	    getdata(dl);
	    printf("          acl[");
	    len2 = le32toh(dl.perm);
	    printf((len2 & (1 << 28)) ? "access" : "default");
	    printf("]");
	    print_perm('=', 'u', -1, len2);
	    print_perm(',', 'g', -1, len2 >> 7);
	    print_perm(',', 'o', -1, len2 >> 14);
	    print_perm(',', 'm', -1, len2 >> 21);
	    len2 = le32toh(dl.count);
	    for (n = 0; n < len2; n++) {
		int type;
		getdata(de);
		type = le32toh(de.type);
		print_perm(',', type & (1 << 28) ? 'g' : 'u',
			   le32toh(de.name), type);
	    }
	    printf("\n");
	    break;
	case SHALL_LOG_XATTR :
	    getdata(dx);
	    len2 = le32toh(dx.namelen);
	    printf("          xattr[%.*s", len2, data);
	    data += len2;
	    len2 = le32toh(dx.valuelen);
	    printf(", %x]=%d[", le32toh(dx.flags), len2);
	    for (n = 0; n < len2; n++) {
		unsigned char c = *data++;
		if (isascii((int)c) && isprint((int)c) && c != '%')
		    putchar(c);
		else
		    printf("%%%02x", c);
	    }
	    printf("]\n");
	    break;
	case SHALL_LOG_HASH :
	    getdata(dc);
	    printf("          id=%d region=%lld:%lld\n",
		   le32toh(dc.fileid), (long long)le64toh(dc.start),
		   (long long)le64toh(dc.length));
	    printf("          data_hash=");
	    for (n = 0; n < SHALL_HASH_LENGTH; n++)
		printf("%02x", dc.hash[n]);
	    printf("\n");
	    break;
	case SHALL_LOG_DATA :
	    getdata(dr);
	    printf("          id=%d region=%lld:%lld\n",
		   le32toh(dr.fileid), (long long)le64toh(dr.start),
		   (long long)le64toh(dr.length));
	    len2 = le64toh(dr.length);
	    printf("          data=");
	    for (n = 0; n < len2; n++) {
		unsigned char c = *data++;
		printf("%02x", c);
	    }
	    printf("\n");
	    break;
    }
    if (op == 0)
	printf("          DEBUG (%.*s:%d) %.*s\n",
	       len2, name2, result, len1, name1);
    return length;
}

static ssize_t read_events(int fd, char * buffer, size_t len) {
    struct shall_devheader dh;
    off_t oldptr = lseek(fd, 0, SEEK_CUR);
    ssize_t nr, done;
    if (oldptr < 0) return -1;
    nr = read(fd, buffer, len);
    if (nr <= 0) return nr;
    done = 0;
    while (done < nr) {
	unsigned int chk;
	memcpy(&dh, buffer + done, sizeof(dh));
	chk = shall_checksum_log(&dh);
	if (chk != le32toh(dh.checksum)) break;
	chk = le32toh(dh.next_header);
	if (chk < sizeof(dh) || done + chk > nr) break;
	done += chk;
    }
    if (done == 0) {
	errno = EINVAL;
	return -1;
    }
    if (done == nr) return done;
    if (lseek(fd, oldptr + done, SEEK_SET) < 0) return -1;
    return done;
}

int main(int argc, char *argv[]) {
    shall_sb_data_t sb;
    const char * pname = strrchr(argv[0], '/');
    const char * errmsg =
	shall_parse_options(argc - 1, argv + 1, options, args);
    int fd, sve;
    if (pname)
	pname++;
    else
	pname = argv[0];
    if (help) {
	shall_print_help(stdout, pname, options, args);
	return 0;
    }
    if (! errmsg && mounted && input)
	errmsg = "Cannot specify both -i and -m";
    if (! errmsg && sbinfo && input)
	errmsg = "Cannot specify both -i and -s";
    if (! errmsg && all_logs && debug_logs)
	errmsg = "Cannot specify both -i and -s";
    if (! errmsg && clear_logs && ! all_logs)
	errmsg = "Cannot specify -c without -l";
    if (! errmsg && clear_logs && mounted)
	errmsg = "Cannot specify -c with -m";
    if (! errmsg && clear_logs && input)
	errmsg = "Cannot specify -c with -i";
    if (errmsg) {
	fprintf(stderr, "%s: %s\nUse \"%s -h\" for help\n",
		pname, errmsg, pname);
	return 1;
    }
    if (mounted) {
	/* search for mounted device */
	struct stat sbuff;
	if (stat(device, &sbuff) < 0) goto out_error;
	if (S_ISDIR(sbuff.st_mode)) {
	    if (! shall_find_device(device, &sbuff.st_rdev)) {
		fprintf(stderr, "%s: cannot find shallfs on %s\n",
			pname, device);
		return 1;
	    }
	} else if (! S_ISBLK(sbuff.st_mode)) {
	    fprintf(stderr, "%s: %s: not a block device or directory\n",
		    pname, device);
	    return 1;
	}
	if (! shall_mounted_info(sbuff.st_rdev, &sb)) {
	    fprintf(stderr, "%s: %s: cannot find mounted shallfs instance\n",
		    pname, device);
	    return 1;
	}
	if (all_logs || debug_logs)
	    fd = shall_open_logfile(sbuff.st_rdev, blocking, debug_prog);
	else
	    fd = 0;
    } else if (input) {
	fd = open(device, O_RDONLY);
	all_logs = 1;
    } else {
	/* open device, doing automatic recovery if necessary */
	fd = shall_open_device(device, ! clear_logs, &sb);
    }
    if (fd < 0) goto out_error;
    if (sbinfo || (! all_logs && ! debug_logs && ! input)) {
	/* print superblock information */
#define print_size(name) \
    printf("    %-12s%12lld (%.1fMB)\n", \
	   #name, (long long)sb.name, (float)sb.name / 1048576.0);
	printf("Superblock information for %s:\n", device);
	printf("    version     %12lld\n", (long long)sb.version);
	print_size(device_size);
	print_size(data_space);
	print_size(data_start);
	print_size(data_length);
	print_size(max_length);
	printf("    num_superblocks %8d\n", sb.num_superblocks);
	printf("    alignment     %10d\n", sb.alignment);
	printf("    flags: %s, %s, %s\n",
	       (sb.flags & SHALL_SB_VALID)  ? "valid"  : "invalid",
	       (sb.flags & SHALL_SB_DIRTY)  ? "dirty"  : "clean",
	       (sb.flags & SHALL_SB_UPDATE) ? "update" : "operation");
#undef print_size
    }
    if (all_logs || input || debug_logs) {
	char buffer[16384];
	FILE * dest;
	off_t where = sb.data_start;
	int count = 0, report = 1;
	if (filename) {
	    dest = fopen(filename, append ? "ab" : "wb");
	    if (! dest) {
		fprintf(stderr, "%s: %s: %s\n",
			pname, filename, strerror(errno));
	    }
	} else {
	    dest = NULL;
	    if (! debug_logs) printf("Events logged in %s:\n", device);
	}
	while (1) {
	    ssize_t nr;
	    if (max_logs > 0 && count > max_logs) break;
	    if (mounted)
		nr = read(fd, buffer, sizeof(buffer));
	    else if (input)
		nr = read_events(fd, buffer, sizeof(buffer));
	    else
		nr = shall_read_logs(fd, &sb, buffer,
				     sizeof(buffer), debug_prog);
	    if (nr == 0 || (nr < 0 && errno == EAGAIN)) break;
	    if (nr < 0) goto out_close;
	    if (dest && ! debug_logs) {
	    	if (fwrite(buffer, nr, 1, dest) < 1) {
		    fprintf(stderr, "%s: %s: %s\n",
			    pname, filename, strerror(errno));
		    report = 0;
		    break;
		}
	    } else {
		int ptr = 0;
		while (ptr < nr) {
		    int next;
		    count++;
		    if (debug_logs)
			next = print_debug_log(dest ? dest : stdout,
					       buffer + ptr);
		    else
			next = print_log(where, buffer + ptr, count);
		    ptr += next;
		    where += next;
		    if (where >= sb.data_space)
			where -= sb.data_space;
		}
	    }
	}
	if (dest) {
	    if (fclose(dest) == EOF) {
		if (report)
		    fprintf(stderr, "%s: %s: %s\n",
			    pname, filename, strerror(errno));
	    }
	} else if (debug_logs) {
	    if (follow) {
		printf("** memory leak?\n");
		while (follow) {
		    follow_t * fw = follow;
		    follow = follow->next;
		    printf("  %lx:%-6ld   %s:%d\n",
			   fw->address, (long)fw->size,
			   fw->filename, fw->lineno);
		    free(fw);
		}
	    }
	} else {
	    printf("End of journal, %d events\n", count);
	}
	if (clear_logs) {
	    struct shall_devsuper dsb;
	    sb.version++;
	    sb.flags &= ~SHALL_SB_DIRTY;
	    shall_init_sb(&dsb, &sb, NULL);
	    shall_write_sb(fd, &dsb, 0);
	    shall_write_sb(fd, &dsb, 1);
	}
    }
    if (fd > 0 && close(fd) < 0) goto out_error;
    return 0;
out_close:
    sve = errno;
    close(fd);
    errno = sve;
out_error:
    fprintf(stderr, "%s: %s: %s\n", pname, device, strerror(errno));
    return 1;
}

