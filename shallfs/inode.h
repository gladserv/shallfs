#ifndef _SHALL_INODE_H
#define _SHALL_INODE_H

struct inode * shall_new_inode(struct super_block *, struct dentry *);

void shall_evict_inode(struct inode *);

extern const struct xattr_handler shall_xattr_handler;
#endif /* _SHALL_INODE_H */
