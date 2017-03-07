#ifndef _SHALL_DEVICE_H_
#define _SHALL_DEVICE_H_

#define SHALL_SB_MAGIC "SHALL 01"
#define SHALL_DEV_BLOCK 4096

/* on-disk superblock format */
struct shall_devsuper {
	char magic1[8];				/*    0: "SHALL 01" */
	__le64 device_size; 			/*    8: total device size */
	__le64 data_space; 			/*   16: total log space */
	__le64 data_start;			/*   24: first byte of data */
	__le64 data_length;			/*   32: total length of logs */
	__le64 max_length;			/*   40: max(data_length) */
	__le64 version;				/*   48: see documentation */
	__le32 flags;				/*   56: see below */
	__le32 alignment;			/*   60: log alignment */
	__le32 num_superblocks;			/*   64: num. of superblocks */
	__le32 this_superblock;			/*   68: this superblock */
	char __reserved0[696];			/*   72: */
	__le64 new_size;			/*  768: see tuneshallfs */
	__le32 new_alignment;			/*  776: see tuneshallfs */
	__le32 new_superblocks;			/*  780: see tuneshallfs */
	char __reserved1[228];			/*  784: */
	char magic2[8];				/* 1012: "SHALL 01" */
	__le32 checksum;			/* 1020: checksum */
} __attribute__((packed));			/* 1024 bytes */

/* size of area to checksum */
#define shall_superblock_checksize offsetof(struct shall_devsuper, checksum)

#define SHALL_SB_OFFSET (SHALL_DEV_BLOCK - sizeof(struct shall_devsuper))

/* superblock flags */
enum shall_sb_flags {
	SHALL_SB_VALID	= 0x0001,		/* always set! */
	SHALL_SB_DIRTY	= 0x0002,		/* not cleanly unmounted */
	SHALL_SB_UPDATE	= 0x0004,		/* update was interrupted */
};

/* on-disk log format */
struct shall_devheader {
	__le32 next_header;			/*   0: offset to next log */
	__le32 operation;			/*   4: operation code */
	__le64 req_sec;				/*   8: request time */
	__le32 req_nsec;			/*  16: request time */
	__le32 result;                          /*  20: result, if available */
	__le32 flags;				/*  24: see below */
	__le32 checksum;			/*  28: checksum */
} __attribute__((packed));			/*  32 bytes + length of data */

/* log flags are defined in <shall/operation.h> */

/* size of area to checksum */
#define shall_devheader_checksize offsetof(struct shall_devheader, checksum)

/* on-disk number (fileid or filename length) format */
struct shall_devfileid {
	__le32 fileid;				/*   0: the number */
} __attribute__((packed));			/*   4 bytes */

/* on-disk large number (file size) format */
struct shall_devsize {
	__le64 size;				/*   0: the size */
} __attribute__((packed));			/*   8 bytes */

/* on-disk region format */
struct shall_devregion {
	__le64 start;				/*   0: start of region */
	__le64 length;				/*   8: length of region */
	__le32 fileid;				/*  16: file ID */
} __attribute__((packed));			/*  20 bytes */

/* on-dist attr format */
struct shall_devattr {
	__le32 flags;				/*   0: flags */
	__le32 mode;				/*   4: file permissions */
	__le32 user;				/*   8: owner */
	__le32 group;				/*  12: owner */
	__le64 size;				/*  16: size, for truncate;
						 *      device, for mknod */
	__le64 atime_sec;			/*  24: atime, seconds */
	__le64 mtime_sec;			/*  32: mtime, seconds */
	__le32 atime_nsec;			/*  40: atime, nanoseconds */
	__le32 mtime_nsec;			/*  44: mtime, nanoseconds */
} __attribute__((packed));			/*  48 bytes */

/* on-disk acl format */
struct shall_devacl_entry {
	__le32 type;				/*   0: entry type/mode */
	__le32 name;				/*   4: user or group ID */
} __attribute__((packed));			/*   8 bytes */

struct shall_devacl {
	__le32 count;				/*   0: number of entries */
	__le32 perm;				/*   4: combined user_obj,
						 *      group_obj, other and
						 *      mask entries; also
						 *      acl type */
	struct shall_devacl_entry entries[0];	/*   8: entries */
} __attribute__((packed));			/*   8 * (count + 1) bytes */

/* acl entry type; this is independent on the actual numbers used in the
 * system's POSIX ACL calls; also, we can store up to four of these and a
 * flag in a single integer */
enum shall_acltype {
	shall_acl_read		= 0x0001,
	shall_acl_write		= 0x0002,
	shall_acl_execute	= 0x0004,
	shall_acl_add		= 0x0008,
	shall_acl_delete	= 0x0010,
	shall_acl_what		= 0x007f,
};

struct shall_devxattr {
	__le32 flags;				/*   0: flags */
	__le32 namelen;				/*   4: length of name */
	__le32 valuelen;			/*   8: length of value */
	char data[0];				/*  12: name, then value */
} __attribute__((packed));			/*  12 + namelen + valuelen
						 *      bytes */

#endif /* _SHALL_DEVICE_H_ */
