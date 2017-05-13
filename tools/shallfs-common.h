#ifndef _SHALL_H_
#define _SHALL_H_

/* types required so we can read the kernel's device.h */
typedef uint64_t __le64;
typedef uint32_t __le32;
typedef uint16_t __le16;

#include <sys/types.h>
#include <shallfs/device.h>

typedef struct {
    char name;
    long * value;
    const char * valname;
    const char * descr;
} shall_options_t;

typedef struct {
    const char ** value;
    const char * valname;
    int required;
    const char * descr;
} shall_args_t;

typedef struct {
    off_t dev_size;
    int num_superblocks;
    int alignment;
} shall_sb_info_t;

typedef struct {
    int64_t version;
    off_t device_size;
    off_t data_space;
    off_t data_start;
    off_t data_length;
    off_t max_length;
    off_t real_start;
    int flags;
    int num_superblocks;
    int this_superblock;
    int alignment;
    int next_superblock;
} shall_sb_data_t;

/* result of checking a superblock */
typedef enum {
    shall_check_ok         = 0x00000000,
    shall_check_novalid    = 0x00000001,  /* SHALL_SB_VALID not present */
    shall_check_ioerr      = 0x00000002,  /* I/O error prevented some checks */
    shall_check_toobig     = 0x00000004,  /* journal larger than device? */
    shall_check_toosmall   = 0x00000008,  /* journal or metadata too small */
    shall_check_nonblock   = 0x00000010,  /* jurnal not multiple of a block */
    shall_check_dataspace  = 0x00000020,  /* data space != calculated */
    shall_check_datastart  = 0x00000040,  /* data start out of range */
    shall_check_datalength = 0x00000080,  /* data length out of range */
    shall_check_maxlength  = 0x00000100,  /* max length out of range */
    shall_check_alignment  = 0x00000200,  /* invalid alignment value */
    shall_check_lastsb     = 0x00000400,  /* last superblock outside device! */
    shall_check_flags      = 0x00000800,  /* flags contain invalid bits */

    shall_check_fixable = shall_check_novalid
		        | shall_check_dataspace
			| shall_check_maxlength
			| shall_check_alignment
			| shall_check_lastsb
			| shall_check_flags
} shall_check_t;

/* parse command line and return error message, NULL if OK */
const char * shall_parse_options(int argc, char *argv[],
				 const shall_options_t * options,
				 const shall_args_t * args);

/* print help text */
void shall_print_help(FILE * F, const char * pname,
		      const shall_options_t * options,
		      const shall_args_t * args);

/* like strtol but always uses default base and accepts a number with unit */
long shall_strtol(const char *, char **);

/* initialise a new superblock structure */
void shall_init_sb(struct shall_devsuper *, const shall_sb_data_t * data,
		   const shall_sb_info_t * change);

/* calculate checksum for superblock structure */
unsigned int shall_checksum_sb(const struct shall_devsuper *);

/* write a superblock to disk; the structure must have been prepared by
 * one of the other functions; return 0 on error, 1 OK */
int shall_write_sb(int fd, struct shall_devsuper *, int which);

/* write all superblocks */
int shall_write_all_sb(int fd, struct shall_devsuper *, int verbose);

/* calculate location of n-th superblock */
static inline off_t shall_superblock_location(int n) {
	return (off_t)n * 4 * SHALL_DEV_BLOCK * (4 * (off_t)n + 1)
	       + SHALL_SB_OFFSET;
}

/* calculate checksum for log header structure */
unsigned int shall_checksum_log(const struct shall_devheader *);

/* find mounted device by underlying path */
int shall_find_device(const char *, dev_t *);

/* open a device and perform any automatic recovery */
int shall_open_device(const char * name, int readonly, shall_sb_data_t *);

/* read a superblock from disk; return 0 on error, 1 OK */
int shall_read_sb(int fd, shall_sb_data_t *, int which);

/* like shall_read_sb but without the consistency checks */
int shall_read_sb_raw(int fd, shall_sb_data_t *, int which);

/* do some consistency checks on a superblock and returns the result */
shall_check_t shall_check_sb(int fd, const shall_sb_data_t *, int which);

/* read events from disk; return amount of buffer used, 0 if EOF, negative
 * if error; if successful, updates superblock information */
ssize_t shall_read_logs(int fd, shall_sb_data_t *, char *, size_t, int);

/* read data from disk; return amount of buffer used, 0 if EOF, negative
 * if error; does not update superblock information, and the last event
 * may be truncated depending on space in the buffer; caller must make
 * sure real_start and next_superblock are correct as this cannot update
 * the superblock to recalculate them */
ssize_t shall_read_data(int fd, const shall_sb_data_t *, char *, size_t, int);

/* advance superblock pointers by given offset */
void shall_advance_pointers(shall_sb_data_t *, size_t);

/* read superblock information from a mounted filesystem */
int shall_mounted_info(dev_t, shall_sb_data_t *);

/* open mounted filesystem's logfile */
int shall_open_logfile(dev_t, int blocking, int verbose);

/* send a command to a mounted filesystem */
int shall_ctrl_commit(dev_t);
int shall_ctrl_clear(dev_t, int);
int shall_ctrl_userlog(dev_t, const char *);

#endif /* _SHALL_H_ */
