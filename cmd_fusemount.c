#include <errno.h>
#include <float.h>
#include <getopt.h>
#include <stdio.h>
#include <sys/statvfs.h>

#include <fuse_lowlevel.h>

#include "cmds.h"
#include "libbcachefs.h"
#include "tools-util.h"

#include "libbcachefs/bcachefs.h"
#include "libbcachefs/btree_iter.h"
#include "libbcachefs/buckets.h"
#include "libbcachefs/dirent.h"
#include "libbcachefs/error.h"
#include "libbcachefs/fs-common.h"
#include "libbcachefs/inode.h"
#include "libbcachefs/opts.h"
#include "libbcachefs/super.h"

/* mode_to_type(): */
#include "libbcachefs/fs.h"

#include <linux/dcache.h>

/* XXX cut and pasted from fsck.c */
#define QSTR(n) { { { .len = strlen(n) } }, .name = n }

static inline u64 map_root_ino(u64 ino)
{
	return ino == 1 ? 4096 : ino;
}

static inline u64 unmap_root_ino(u64 ino)
{
	return ino == 4096 ? 1 : ino;
}

static struct stat inode_to_stat(struct bch_fs *c,
				 struct bch_inode_unpacked *bi)
{
	return (struct stat) {
		.st_size	= bi->bi_size,
		.st_mode	= bi->bi_mode,
		.st_uid		= bi->bi_uid,
		.st_gid		= bi->bi_gid,
		.st_nlink	= bch2_inode_nlink_get(bi),
		.st_rdev	= bi->bi_dev,
		.st_blksize	= block_bytes(c),
		.st_blocks	= bi->bi_sectors,
		.st_atim	= bch2_time_to_timespec(c, bi->bi_atime),
		.st_mtim	= bch2_time_to_timespec(c, bi->bi_mtime),
		.st_ctim	= bch2_time_to_timespec(c, bi->bi_ctime),
	};
}

static struct fuse_entry_param inode_to_entry(struct bch_fs *c,
					      struct bch_inode_unpacked *bi)
{
	return (struct fuse_entry_param) {
		.ino		= bi->bi_inum,
		.generation	= bi->bi_generation,
		.attr		= inode_to_stat(c, bi),
		.attr_timeout	= DBL_MAX,
		.entry_timeout	= DBL_MAX,
	};
}

static void bcachefs_fuse_destroy(void *arg)
{
	struct bch_fs *c = arg;

	bch2_fs_stop(c);
}

static void bcachefs_fuse_lookup(fuse_req_t req, fuse_ino_t dir,
				 const char *name)
{
	struct bch_fs *c = fuse_req_userdata(req);
	struct bch_inode_unpacked bi;
	struct qstr qstr = QSTR(name);
	u64 inum;
	int ret;

	dir = map_root_ino(dir);

	pr_info("dir %llu name %s", (u64) dir, name);

	inum = bch2_dirent_lookup(c, dir, &qstr);
	if (!inum) {
		ret = -ENOENT;
		goto err;
	}

	ret = bch2_inode_find_by_inum(c, inum, &bi);
	if (ret)
		goto err;

	bi.bi_inum = unmap_root_ino(bi.bi_inum);

	struct fuse_entry_param e = inode_to_entry(c, &bi);
	fuse_reply_entry(req, &e);
	return;
err:
	fuse_reply_err(req, -ret);
}

static void bcachefs_fuse_getattr(fuse_req_t req, fuse_ino_t inum,
				  struct fuse_file_info *fi)
{
	struct bch_fs *c = fuse_req_userdata(req);
	struct bch_inode_unpacked bi;
	struct stat attr;
	int ret;

	inum = map_root_ino(inum);

	pr_info("inum %llu", (u64) inum);

	ret = bch2_inode_find_by_inum(c, inum, &bi);
	if (ret) {
		fuse_reply_err(req, -ret);
		return;
	}

	bi.bi_inum = unmap_root_ino(bi.bi_inum);

	attr = inode_to_stat(c, &bi);
	fuse_reply_attr(req, &attr, DBL_MAX);
}

static void bcachefs_fuse_setattr(fuse_req_t req, fuse_ino_t inum,
				  struct stat *attr, int to_set,
				  struct fuse_file_info *fi)
{
	struct bch_fs *c = fuse_req_userdata(req);
	struct bch_inode_unpacked inode_u;
	struct btree_trans trans;
	struct btree_iter *iter;
	u64 now;
	int ret;

	inum = map_root_ino(inum);

	bch2_trans_init(&trans, c, 0, 0);
retry:
	bch2_trans_begin(&trans);
	now = bch2_current_time(c);

	iter = bch2_inode_peek(&trans, &inode_u, inum, BTREE_ITER_INTENT);
	ret = PTR_ERR_OR_ZERO(iter);
	if (ret)
		goto err;

	if (to_set & FUSE_SET_ATTR_MODE)
		inode_u.bi_mode	= attr->st_mode;
	if (to_set & FUSE_SET_ATTR_UID)
		inode_u.bi_uid	= attr->st_uid;
	if (to_set & FUSE_SET_ATTR_GID)
		inode_u.bi_gid	= attr->st_gid;
	if (to_set & FUSE_SET_ATTR_SIZE)
		inode_u.bi_size	= attr->st_size;
	if (to_set & FUSE_SET_ATTR_ATIME)
		inode_u.bi_atime = timespec_to_bch2_time(c, attr->st_atim);
	if (to_set & FUSE_SET_ATTR_MTIME)
		inode_u.bi_mtime = timespec_to_bch2_time(c, attr->st_mtim);
	if (to_set & FUSE_SET_ATTR_ATIME_NOW)
		inode_u.bi_atime = now;
	if (to_set & FUSE_SET_ATTR_MTIME_NOW)
		inode_u.bi_mtime = now;

	ret   = bch2_inode_write(&trans, iter, &inode_u) ?:
		bch2_trans_commit(&trans, NULL, NULL,
				  BTREE_INSERT_ATOMIC|
				  BTREE_INSERT_NOFAIL);
err:
	if (ret == -EINTR)
		goto retry;

	bch2_trans_exit(&trans);

	if (!ret) {
		*attr = inode_to_stat(c, &inode_u);
		fuse_reply_attr(req, attr, DBL_MAX);
	} else {
		fuse_reply_err(req, -ret);
	}
}

static void bcachefs_fuse_readlink(fuse_req_t req, fuse_ino_t inum)
{
	//struct bch_fs *c = fuse_req_userdata(req);

	//char *link = malloc();

	//fuse_reply_readlink(req, link);
}

static int do_create(struct bch_fs *c, u64 dir,
		     const char *name, mode_t mode, dev_t rdev,
		     struct bch_inode_unpacked *new_inode)
{
	struct qstr qstr = QSTR(name);
	struct bch_inode_unpacked dir_u;

	dir = map_root_ino(dir);

	bch2_inode_init_early(c, new_inode);

	return bch2_trans_do(c, NULL, 0,
			bch2_create_trans(&trans,
				dir, &dir_u,
				new_inode, &qstr,
				0, 0, mode, rdev, NULL, NULL));
}

static void bcachefs_fuse_mknod(fuse_req_t req, fuse_ino_t dir,
				const char *name, mode_t mode,
				dev_t rdev)
{
	struct bch_fs *c = fuse_req_userdata(req);
	struct bch_inode_unpacked new_inode;
	int ret;

	ret = do_create(c, dir, name, mode, rdev, &new_inode);
	if (ret)
		goto err;

	struct fuse_entry_param e = inode_to_entry(c, &new_inode);
	fuse_reply_entry(req, &e);
	return;
err:
	fuse_reply_err(req, -ret);
}

static void bcachefs_fuse_mkdir(fuse_req_t req, fuse_ino_t dir,
				const char *name, mode_t mode)
{
	bcachefs_fuse_mknod(req, dir, name, mode, 0);
}

static void bcachefs_fuse_unlink(fuse_req_t req, fuse_ino_t dir,
				 const char *name)
{
	struct bch_fs *c = fuse_req_userdata(req);
	struct bch_inode_unpacked dir_u, inode_u;
	struct qstr qstr = QSTR(name);
	int ret;

	dir = map_root_ino(dir);

	ret = bch2_trans_do(c, NULL, BTREE_INSERT_ATOMIC|BTREE_INSERT_NOFAIL,
			    bch2_unlink_trans(&trans, dir, &dir_u,
					      &inode_u, &qstr));

	fuse_reply_err(req, -ret);
}

static void bcachefs_fuse_rmdir(fuse_req_t req, fuse_ino_t dir,
				const char *name)
{
	dir = map_root_ino(dir);

	bcachefs_fuse_unlink(req, dir, name);
}

#if 0
static void bcachefs_fuse_symlink(fuse_req_t req, const char *link,
				  fuse_ino_t parent, const char *name)
{
	struct bch_fs *c = fuse_req_userdata(req);
}
#endif

static void bcachefs_fuse_rename(fuse_req_t req,
				 fuse_ino_t src_dir, const char *srcname,
				 fuse_ino_t dst_dir, const char *dstname,
				 unsigned flags)
{
	struct bch_fs *c = fuse_req_userdata(req);
	struct bch_inode_unpacked dst_dir_u, src_dir_u;
	struct bch_inode_unpacked src_inode_u, dst_inode_u;
	struct qstr dst_name = QSTR(srcname);
	struct qstr src_name = QSTR(dstname);
	int ret;

	src_dir = map_root_ino(src_dir);
	dst_dir = map_root_ino(dst_dir);

	/* XXX handle overwrites */
	ret = bch2_trans_do(c, NULL, BTREE_INSERT_ATOMIC,
		bch2_rename_trans(&trans,
				  src_dir, &src_dir_u,
				  dst_dir, &dst_dir_u,
				  &src_inode_u, &dst_inode_u,
				  &src_name, &dst_name,
				  BCH_RENAME));

	fuse_reply_err(req, -ret);
}

static void bcachefs_fuse_link(fuse_req_t req, fuse_ino_t inum,
			       fuse_ino_t newparent, const char *newname)
{
	struct bch_fs *c = fuse_req_userdata(req);
	struct bch_inode_unpacked inode_u;
	struct qstr qstr = QSTR(newname);
	int ret;

	ret = bch2_trans_do(c, NULL, BTREE_INSERT_ATOMIC,
			    bch2_link_trans(&trans, newparent,
					    inum, &inode_u, &qstr));

	if (!ret) {
		struct fuse_entry_param e = inode_to_entry(c, &inode_u);
		fuse_reply_entry(req, &e);
	} else {
		fuse_reply_err(req, -ret);
	}
}

#if 0
static void bcachefs_fuse_open(fuse_req_t req, fuse_ino_t inum,
			       struct fuse_file_info *fi)
{
	struct bch_fs *c = fuse_req_userdata(req);
}

static void bcachefs_fuse_read(fuse_req_t req, fuse_ino_t inum,
			       size_t size, off_t off,
			       struct fuse_file_info *fi)
{
	struct bch_fs *c = fuse_req_userdata(req);
}

static void bcachefs_fuse_flush(fuse_req_t req, fuse_ino_t inum,
				struct fuse_file_info *fi)
{
	struct bch_fs *c = fuse_req_userdata(req);
}

static void bcachefs_fuse_release(fuse_req_t req, fuse_ino_t inum,
				  struct fuse_file_info *fi)
{
	struct bch_fs *c = fuse_req_userdata(req);
}

static void bcachefs_fuse_fsync(fuse_req_t req, fuse_ino_t inum, int datasync,
				struct fuse_file_info *fi)
{
	struct bch_fs *c = fuse_req_userdata(req);
}

static void bcachefs_fuse_opendir(fuse_req_t req, fuse_ino_t inum,
				  struct fuse_file_info *fi)
{
	struct bch_fs *c = fuse_req_userdata(req);
}
#endif

struct fuse_dir_context {
	struct dir_context	ctx;
	fuse_req_t		req;
	char			*buf;
	size_t			bufsize;
};

static int fuse_filldir(struct dir_context *_ctx,
			const char *name, int namelen,
			loff_t pos, u64 dir, unsigned type)
{
	struct fuse_dir_context *ctx =
		container_of(_ctx, struct fuse_dir_context, ctx);

	struct stat statbuf = {
		.st_ino		= map_root_ino(dir),
		.st_mode	= type << 12,
	};

	size_t len = fuse_add_direntry(ctx->req,
				       ctx->buf,
				       ctx->bufsize,
				       name,
				       &statbuf,
				       pos + 1);

	if (len > ctx->bufsize)
		return 0;

	ctx->buf	+= len;
	ctx->bufsize	-= len;
	return 1;
}

static void bcachefs_fuse_readdir(fuse_req_t req, fuse_ino_t dir,
				  size_t size, off_t off,
				  struct fuse_file_info *fi)
{
	struct bch_fs *c = fuse_req_userdata(req);
	char buf[4096];
	struct fuse_dir_context ctx = {
		.ctx.actor	= fuse_filldir,
		.ctx.pos	= off,
		.req		= req,
		.buf		= buf,
		.bufsize	= sizeof(buf),
	};
	int ret;

	dir = map_root_ino(dir);

	ret = bch2_readdir(c, dir, &ctx.ctx);
	if (!ret) {
		fuse_reply_buf(req, buf, ctx.buf - buf);
	} else {
		fuse_reply_err(req, -ret);
	}
}

#if 0
static void bcachefs_fuse_releasedir(fuse_req_t req, fuse_ino_t inum,
				     struct fuse_file_info *fi)
{
	struct bch_fs *c = fuse_req_userdata(req);
}

static void bcachefs_fuse_fsyncdir(fuse_req_t req, fuse_ino_t inum, int datasync,
				   struct fuse_file_info *fi)
{
	struct bch_fs *c = fuse_req_userdata(req);
}
#endif

static void bcachefs_fuse_statfs(fuse_req_t req, fuse_ino_t inum)
{
	struct bch_fs *c = fuse_req_userdata(req);
	struct bch_fs_usage_short usage = bch2_fs_usage_read_short(c);
	unsigned shift = c->block_bits;
	struct statvfs statbuf = {
		.f_bsize	= block_bytes(c),
		.f_frsize	= block_bytes(c),
		.f_blocks	= usage.capacity >> shift,
		.f_bfree	= (usage.capacity - usage.used) >> shift,
		//.f_bavail	= statbuf.f_bfree,
		.f_files	= usage.nr_inodes,
		.f_ffree	= U64_MAX,
		.f_namemax	= BCH_NAME_MAX,
	};

	fuse_reply_statfs(req, &statbuf);
}

#if 0
static void bcachefs_fuse_setxattr(fuse_req_t req, fuse_ino_t inum,
				   const char *name, const char *value,
				   size_t size, int flags)
{
	struct bch_fs *c = fuse_req_userdata(req);
}

static void bcachefs_fuse_getxattr(fuse_req_t req, fuse_ino_t inum,
				   const char *name, size_t size)
{
	struct bch_fs *c = fuse_req_userdata(req);

	fuse_reply_xattr(req, );
}

static void bcachefs_fuse_listxattr(fuse_req_t req, fuse_ino_t inum, size_t size)
{
	struct bch_fs *c = fuse_req_userdata(req);
}

static void bcachefs_fuse_removexattr(fuse_req_t req, fuse_ino_t inum,
				      const char *name)
{
	struct bch_fs *c = fuse_req_userdata(req);
}
#endif

static void bcachefs_fuse_create(fuse_req_t req, fuse_ino_t dir,
				 const char *name, mode_t mode,
				 struct fuse_file_info *fi)
{
	struct bch_fs *c = fuse_req_userdata(req);
	struct bch_inode_unpacked new_inode;
	int ret;

	ret = do_create(c, dir, name, mode, 0, &new_inode);
	if (ret)
		goto err;

	struct fuse_entry_param e = inode_to_entry(c, &new_inode);
	fuse_reply_create(req, &e, fi);
	return;
err:
	fuse_reply_err(req, -ret);

}

#if 0
static void bcachefs_fuse_write_buf(fuse_req_t req, fuse_ino_t inum,
				    struct fuse_bufvec *bufv, off_t off,
				    struct fuse_file_info *fi)
{
	struct bch_fs *c = fuse_req_userdata(req);
}

static void bcachefs_fuse_fallocate(fuse_req_t req, fuse_ino_t inum, int mode,
				    off_t offset, off_t length,
				    struct fuse_file_info *fi)
{
	struct bch_fs *c = fuse_req_userdata(req);
}
#endif

static const struct fuse_lowlevel_ops bcachefs_fuse_ops = {
	.destroy	= bcachefs_fuse_destroy,
	.lookup		= bcachefs_fuse_lookup,
	.getattr	= bcachefs_fuse_getattr,
	.setattr	= bcachefs_fuse_setattr,
	.readlink	= bcachefs_fuse_readlink,
	.mknod		= bcachefs_fuse_mknod,
	.mkdir		= bcachefs_fuse_mkdir,
	.unlink		= bcachefs_fuse_unlink,
	.rmdir		= bcachefs_fuse_rmdir,
	//.symlink	= bcachefs_fuse_symlink,
	.rename		= bcachefs_fuse_rename,
	.link		= bcachefs_fuse_link,
	//.open		= bcachefs_fuse_open,
	//.read		= bcachefs_fuse_read,
	//.write	= bcachefs_fuse_write,
	//.flush	= bcachefs_fuse_flush,
	//.release	= bcachefs_fuse_release,
	//.fsync	= bcachefs_fuse_fsync,
	//.opendir	= bcachefs_fuse_opendir,
	.readdir	= bcachefs_fuse_readdir,
	//.releasedir	= bcachefs_fuse_releasedir,
	//.fsyncdir	= bcachefs_fuse_fsyncdir,
	.statfs		= bcachefs_fuse_statfs,
	//.setxattr	= bcachefs_fuse_setxattr,
	//.getxattr	= bcachefs_fuse_getxattr,
	//.listxattr	= bcachefs_fuse_listxattr,
	//.removexattr	= bcachefs_fuse_removexattr,
	.create		= bcachefs_fuse_create,

	/* posix locks: */
#if 0
	.getlk		= bcachefs_fuse_getlk,
	.setlk		= bcachefs_fuse_setlk,
#endif
	//.write_buf	= bcachefs_fuse_write_buf,
	//.fallocate	= bcachefs_fuse_fallocate,

};

int cmd_fusemount(int argc, char *argv[])
{
	struct bch_opts bch_opts = bch2_opts_empty();
	struct bch_fs *c = NULL;

	c = bch2_fs_open(argv + optind, argc - optind, bch_opts);
	if (IS_ERR(c))
		die("error opening %s: %s", argv[optind],
		    strerror(-PTR_ERR(c)));

	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	struct fuse_cmdline_opts fuse_opts;
	if (fuse_parse_cmdline(&args, &fuse_opts) < 0)
		die("fuse_parse_cmdline err: %m");

	struct fuse_session *se =
		fuse_session_new(&args, &bcachefs_fuse_ops,
				 sizeof(bcachefs_fuse_ops), c);
	if (!se)
		die("fuse_lowlevel_new err: %m");

	if (fuse_set_signal_handlers(se) < 0)
		die("fuse_set_signal_handlers err: %m");

	if (fuse_session_mount(se, "/home/kent/mnt"))
		die("fuse_mount err: %m");

	int ret = fuse_session_loop(se);

	fuse_session_unmount(se);
	fuse_remove_signal_handlers(se);
	fuse_session_destroy(se);
	fuse_opt_free_args(&args);

	return ret ? 1 : 0;
}
