/* SPDX-License-Identifier: LGPL-2.1 */
/*
 *
 *   Copyright (c) International Business Machines  Corp., 2002, 2007
 *   Author(s): Steve French (sfrench@us.ibm.com)
 *
 */

#ifndef _CIFSFS_H
#define _CIFSFS_H

#include <linux/hash.h>

#define ROOT_I 2

/*
 * ino_t is 32-bits on 32-bit arch. We have to squash the 64-bit value down
 * so that it will fit. We use hash_64 to convert the value to 31 bits, and
 * then add 1, to ensure that we don't end up with a 0 as the value.
 */
static inline ino_t
cifs_uniqueid_to_ino_t(u64 fileid)
{
	if ((sizeof(ino_t)) < (sizeof(u64)))
		return (ino_t)hash_64(fileid, (sizeof(ino_t) * 8) - 1) + 1;

	return (ino_t)fileid;

}

static inline void cifs_set_time(struct dentry *dentry, unsigned long time)
{
	dentry->d_fsdata = (void *) time;
}

static inline unsigned long cifs_get_time(struct dentry *dentry)
{
	return (unsigned long) dentry->d_fsdata;
}

extern struct file_system_type cifs_fs_type, smb3_fs_type;
extern const struct address_space_operations cifs_addr_ops;
extern const struct address_space_operations cifs_addr_ops_smallbuf;

/* Functions related to super block operations */
extern void cifs_sb_active(struct super_block *sb);
extern void cifs_sb_deactive(struct super_block *sb);

/* Functions related to inodes */
extern const struct inode_operations cifs_dir_inode_ops;
extern struct inode *cifs_root_iget(struct super_block *);
extern int cifs_create(struct mnt_idmap *, struct inode *,
		       struct dentry *, umode_t, bool excl);
extern int cifs_atomic_open(struct inode *, struct dentry *,
			    struct file *, unsigned, umode_t);
extern struct dentry *cifs_lookup(struct inode *, struct dentry *,
				  unsigned int);
extern int cifs_unlink(struct inode *dir, struct dentry *dentry);
extern int cifs_hardlink(struct dentry *, struct inode *, struct dentry *);
extern int cifs_mknod(struct mnt_idmap *, struct inode *, struct dentry *,
		      umode_t, dev_t);
extern struct dentry *cifs_mkdir(struct mnt_idmap *, struct inode *, struct dentry *,
				 umode_t);
extern int cifs_rmdir(struct inode *, struct dentry *);
extern int cifs_rename2(struct mnt_idmap *, struct inode *,
			struct dentry *, struct inode *, struct dentry *,
			unsigned int);
extern int cifs_revalidate_file_attr(struct file *filp);
extern int cifs_revalidate_dentry_attr(struct dentry *);
extern int cifs_revalidate_file(struct file *filp);
extern int cifs_revalidate_dentry(struct dentry *);
extern int cifs_revalidate_mapping(struct inode *inode);
extern int cifs_zap_mapping(struct inode *inode);
extern int cifs_getattr(struct mnt_idmap *, const struct path *,
			struct kstat *, u32, unsigned int);
extern int cifs_setattr(struct mnt_idmap *, struct dentry *,
			struct iattr *);
extern int cifs_fiemap(struct inode *, struct fiemap_extent_info *, u64 start,
		       u64 len);

extern const struct inode_operations cifs_file_inode_ops;
extern const struct inode_operations cifs_symlink_inode_ops;
extern const struct inode_operations cifs_namespace_inode_operations;


/* Functions related to files and directories */
extern const struct netfs_request_ops cifs_req_ops;
extern const struct file_operations cifs_file_ops;
extern const struct file_operations cifs_file_direct_ops; /* if directio mnt */
extern const struct file_operations cifs_file_strict_ops; /* if strictio mnt */
extern const struct file_operations cifs_file_nobrl_ops; /* no brlocks */
extern const struct file_operations cifs_file_direct_nobrl_ops;
extern const struct file_operations cifs_file_strict_nobrl_ops;
extern int cifs_open(struct inode *inode, struct file *file);
extern int cifs_close(struct inode *inode, struct file *file);
extern int cifs_closedir(struct inode *inode, struct file *file);
extern ssize_t cifs_strict_readv(struct kiocb *iocb, struct iov_iter *to);
extern ssize_t cifs_strict_writev(struct kiocb *iocb, struct iov_iter *from);
ssize_t cifs_file_write_iter(struct kiocb *iocb, struct iov_iter *from);
ssize_t cifs_loose_read_iter(struct kiocb *iocb, struct iov_iter *iter);
extern int cifs_flock(struct file *pfile, int cmd, struct file_lock *plock);
extern int cifs_lock(struct file *, int, struct file_lock *);
extern int cifs_fsync(struct file *, loff_t, loff_t, int);
extern int cifs_strict_fsync(struct file *, loff_t, loff_t, int);
extern int cifs_flush(struct file *, fl_owner_t id);
int cifs_file_mmap_prepare(struct vm_area_desc *desc);
int cifs_file_strict_mmap_prepare(struct vm_area_desc *desc);
extern const struct file_operations cifs_dir_ops;
extern int cifs_readdir(struct file *file, struct dir_context *ctx);

/* Functions related to dir entries */
extern const struct dentry_operations cifs_dentry_ops;
extern const struct dentry_operations cifs_ci_dentry_ops;

extern struct vfsmount *cifs_d_automount(struct path *path);

/* Functions related to symlinks */
extern const char *cifs_get_link(struct dentry *, struct inode *,
			struct delayed_call *);
extern int cifs_symlink(struct mnt_idmap *idmap, struct inode *inode,
			struct dentry *direntry, const char *symname);

#ifdef CONFIG_CIFS_XATTR
extern const struct xattr_handler * const cifs_xattr_handlers[];
extern ssize_t	cifs_listxattr(struct dentry *, char *, size_t);
#else
# define cifs_xattr_handlers NULL
# define cifs_listxattr NULL
#endif

extern ssize_t cifs_file_copychunk_range(unsigned int xid,
					struct file *src_file, loff_t off,
					struct file *dst_file, loff_t destoff,
					size_t len, unsigned int flags);

extern long cifs_ioctl(struct file *filep, unsigned int cmd, unsigned long arg);
extern void cifs_setsize(struct inode *inode, loff_t offset);

struct smb3_fs_context;
extern struct dentry *cifs_smb3_do_mount(struct file_system_type *fs_type,
					 int flags, struct smb3_fs_context *ctx);

#ifdef CONFIG_CIFS_NFSD_EXPORT
extern const struct export_operations cifs_export_ops;
#endif /* CONFIG_CIFS_NFSD_EXPORT */

/* when changing internal version - update following two lines at same time */
#define SMB3_PRODUCT_BUILD 55
#define CIFS_VERSION   "2.55"
#endif				/* _CIFSFS_H */
