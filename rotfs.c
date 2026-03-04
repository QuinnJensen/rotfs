#define FUSE_USE_VERSION 31

#include <fuse3/fuse.h>
#include <fuse3/fuse_opt.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <limits.h>
#include <dirent.h>

static char *backing_root = NULL;

/* Join backing_root + path into full path */
static void fullpath(char *buf, const char *path)
{
	snprintf(buf, PATH_MAX, "%s%s", backing_root, path);
}

/* Generate backup name: "path.YYYYMMDD-HHMMSS" */
static void make_backup_name(char *dst, size_t dstsz, const char *real)
{
	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);

	char ts[32];
	strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", &tm);

	snprintf(dst, dstsz, "%s.%s", real, ts);
}

static int maybe_backup_existing(const char *path)
{
	char real[PATH_MAX];
	fullpath(real, path);

	fprintf(stderr, "BACKUP? %s\n", real);

	struct stat st;
	if (lstat(real, &st) == -1) {
		if (errno == ENOENT)
			return 0; /* nothing to back up */
		return -errno;
	}

	if (!S_ISREG(st.st_mode) || st.st_size == 0)
		return 0;

	char backup_real[PATH_MAX];
	make_backup_name(backup_real, sizeof backup_real, real);

	if (rename(real, backup_real) == -1)
		return -errno;

	return 0;
}

/* getattr simply proxies to backing FS */
static int rotfs_getattr(const char *path, struct stat *stbuf,
						 struct fuse_file_info *fi)
{
	(void) fi;
	char real[PATH_MAX];
	fullpath(real, path);

	if (lstat(real, stbuf) == -1)
		return -errno;

	return 0;
}

/* readdir proxied */
static int rotfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
						 off_t offset, struct fuse_file_info *fi,
						 enum fuse_readdir_flags flags)
{
	(void) offset;
	(void) fi;
	(void) flags;

	char real[PATH_MAX];
	fullpath(real, path);

	DIR *dp = opendir(real);
	if (!dp)
		return -errno;

	struct dirent *de;
	while ((de = readdir(dp)) != NULL) {
		struct stat st;
		memset(&st, 0, sizeof st);
		st.st_ino = de->d_ino;
		st.st_mode = de->d_type << 12;

		if (filler(buf, de->d_name, &st, 0, 0))
			break;
	}
	closedir(dp);
	return 0;
}

static int promote_ghost(const char *path);
/*
 * open: if opening for write and (O_TRUNC or O_CREAT without O_EXCL),
 * attempt to back up existing file before creating/truncating.
 */
static int rotfs_open(const char *path, struct fuse_file_info *fi)
{
	int flags = fi->flags;
	int writeish = (flags & (O_WRONLY | O_RDWR)) != 0;
	if (!writeish && (flags & O_CREAT)) writeish = 1;

	fprintf(stderr, "OPEN path=%s flags=0x%x\n", path, flags);

	if (writeish) {
		/* 1. Promote ghost, if this was an unlink-then-recreate case */
		int r = promote_ghost(path);
		if (r < 0)
		    return r;

		/* 2. Normal versioning: if a real file exists, back it up */
		int res = maybe_backup_existing(path);
		fprintf(stderr, " backup result: %d\n\n", res);
		if (res < 0)
			return res;

		/* 4. Ensure we create a new file when overwriting */
		flags |= O_CREAT | O_TRUNC;
	}

	char real[PATH_MAX];
	fullpath(real, path);
	fprintf(stderr, "  open(%s, 0x%x)\n", real, flags);

	int fd = open(real, flags, 0666);
	if (fd == -1) {
		fprintf(stderr, "  open FAILED: errno=%d\n", errno);
		return -errno;
	}

	fi->fh = fd;
	return 0;
}

static int rotfs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	fprintf(stderr, "CREATE path=%s flags=0x%x mode=%o\n", path, fi->flags, mode);

	return rotfs_open(path, fi);
}

static int rotfs_read(const char *path, char *buf, size_t size,
					  off_t offset, struct fuse_file_info *fi)
{
	(void) path;
	ssize_t res = pread(fi->fh, buf, size, offset);
	if (res == -1)
		return -errno;
	return res;
}

static int rotfs_write(const char *path, const char *buf, size_t size,
					   off_t offset, struct fuse_file_info *fi)
{
	(void) path;
	ssize_t res = pwrite(fi->fh, buf, size, offset);
	if (res == -1)
		return -errno;
	return res;
}

static int rotfs_truncate(const char *path, off_t size,
						  struct fuse_file_info *fi)
{
	int res;

	printf(stderr, "TRUNCATE %s size=%11d\n", path, (long long)size);

	if (fi != NULL) {
		res = ftruncate(fi->fh, size);
	} else {
		char real[PATH_MAX];
		fullpath(real, path);
		res = truncate(real, size);
	}

	if (res == -1)
		return -errno;
	return 0;
}

static int rotfs_release(const char *path, struct fuse_file_info *fi)
{
	(void) path;
	close(fi->fh);
	return 0;
}

/* Basic unlink, mkdir, etc., just pass-through; add what you need */

static void make_ghost_name(char *dst, size_t dstsz, const char *real)
{
	// real: /backing/foo  -> ghost: /backing/.foo~unlinked
	const char *base = strrchr(real, '/');
	const char *name = base ? base + 1 : real;
	size_t dir_len = base ? (size_t)(base - real + 1) : 0;

	snprintf(dst, dstsz, "%.*s.%s~unlinked",
			 (int)dir_len, real, name);
}

static int rotfs_unlink(const char *path)
{
	char real[PATH_MAX];
	fullpath(real, path);

	struct stat st;
	if (lstat(real, &st) == -1)
		return -errno;

	if (!S_ISREG(st.st_mode))
		return -EPERM; /* or passthrough unlink if you prefer */

	char ghost[PATH_MAX];
	make_ghost_name(ghost, sizeof ghost, real);

	fprintf(stderr, "UNLINK path=%s real=%s ghost=%s\n", path, real, ghost);

	/* Overwrite any existing ghost with the latest contents */
	if (rename(real, ghost) == -1)
		return -errno;

	return 0;
}

static int promote_ghost(const char *path)
{
	char real[PATH_MAX];
	fullpath(real, path);

	// Split dir and name
	char dir[PATH_MAX], name[PATH_MAX];
	char *slash = strrchr(real, '/');
	if (slash) {
		size_t dlen = (size_t)(slash - real + 1);
		memcpy(dir, real, dlen);
		dir[dlen] = '\0';
		strcpy(name, slash + 1);
	} else {
		strcpy(dir, "./");
		strcpy(name, real);
	}

	// Ghost name: dir/.name~unlinked
	char ghost[PATH_MAX];
	snprintf(ghost, sizeof ghost, "%s.%s~unlinked", dir, name);

	struct stat st;
	if (lstat(ghost, &st) == -1) {
		if (errno == ENOENT) {
			fprintf(stderr, "promote_ghost: no ghost %s\n", ghost);
			return 0;  // no ghost to promote
		}
		fprintf(stderr, "promote_ghost: lstat(%s) failed errno=%d\n", ghost, errno);
		return -errno;
	}

	// Visible backup: dir/name-YYYYMMDD-HHMMSS
	char backup[PATH_MAX];
	make_backup_name(backup, sizeof backup, real);

	fprintf(stderr, "promote_ghost: %s -> %s\n", ghost, backup);
	if (rename(ghost, backup) == -1)
		return -errno;

	return 0;
}

static int rotfs_mkdir(const char *path, mode_t mode)
{
	char real[PATH_MAX];
	fullpath(real, path);
	if (mkdir(real, mode) == -1)
		return -errno;
	return 0;
}

static int rotfs_chmod(const char *path, mode_t mode,
					   struct fuse_file_info *fi)
{
	(void) fi;
	char real[PATH_MAX];
	fullpath(real, path);
	if (chmod(real, mode) == -1)
		return -errno;
	return 0;
}

static int rotfs_chown(const char *path, uid_t uid, gid_t gid,
					   struct fuse_file_info *fi)
{
	(void) fi;
	char real[PATH_MAX];
	fullpath(real, path);
	if (lchown(real, uid, gid) == -1)
		return -errno;
	return 0;
}

static int rotfs_utimens(const char *path, const struct timespec tv[2],
						 struct fuse_file_info *fi)
{
	(void) fi;
	char real[PATH_MAX];
	fullpath(real, path);
	if (utimensat(AT_FDCWD, real, tv, 0) == -1)
		return -errno;
	return 0;
}

static int rotfs_setxattr(const char *path, const char *name,
						  const char *value, size_t size, int flags)
{
	char real[PATH_MAX];
	fullpath(real, path);
	if (lsetxattr(real, name, value, size, flags) == -1)
		return -errno;
	return 0;
}

static int rotfs_getxattr(const char *path, const char *name,
						  char *value, size_t size)
{
	char real[PATH_MAX];
	fullpath(real, path);
	ssize_t res = lgetxattr(real, name, value, size);
	if (res == -1)
		return -errno;
	return res;
}

static int rotfs_listxattr(const char *path, char *list, size_t size)
{
	char real[PATH_MAX];
	fullpath(real, path);
	ssize_t res = llistxattr(real, list, size);
	if (res == -1)
		return -errno;
	return res;
}

static int rotfs_removexattr(const char *path, const char *name)
{
	char real[PATH_MAX];
	fullpath(real, path);
	if (lremovexattr(real, name) == -1)
		return -errno;
	return 0;
}

static int rotfs_rename(const char *from, const char *to, unsigned int flags)
{
	char real_from[PATH_MAX];
	char real_to[PATH_MAX];

	fullpath(real_from, from);
	fullpath(real_to, to);

	fprintf(stderr, "RENAME %s -> %s flags=0x%x\n", from, to, flags);

	/* Handle RENAME_NOREPLACE / RENAME_EXCHANGE if you care about them later;
	   for now assume flags == 0 (plain mv semantics). */

	struct stat st_to;
	int dest_exists = (lstat(real_to, &st_to) == 0) && S_ISREG(st_to.st_mode);

	if (dest_exists) {
		/* Version the existing destination file */
		char backup_to[PATH_MAX];
		make_backup_name(backup_to, sizeof backup_to, real_to);

		if (rename(real_to, backup_to) == -1)
			return -errno;
	}

	/* Now move source to destination */
	if (rename(real_from, real_to) == -1)
		return -errno;

	return 0;
}

static struct fuse_operations rotfs_ops = {
	.getattr	= rotfs_getattr,
	.readdir	= rotfs_readdir,
	.open		= rotfs_open,
	.create		= rotfs_create,
	.read		= rotfs_read,
	.write		= rotfs_write,
	.truncate	= rotfs_truncate,
	.release	= rotfs_release,
	.unlink		= rotfs_unlink,
	.mkdir		= rotfs_mkdir,
	.chmod		= rotfs_chmod,
	.chown		= rotfs_chown,
	.utimens	= rotfs_utimens,
	.setxattr	= rotfs_setxattr,
	.getxattr	= rotfs_getxattr,
	.listxattr	= rotfs_listxattr,
	.removexattr	= rotfs_removexattr,
	.rename		= rotfs_rename,
};

static int rotfs_opt_proc(void *data, const char *arg, int key,
						  struct fuse_args *outargs)
{
	(void) data;
	(void) key;

	if (strncmp(arg, "--backing=", 10) == 0) {
		const char *val = arg + 10;
		char *abs = realpath(val, NULL);
		if (!abs) {
			perror("realpath(--backing=)");
			exit(1);
		}
		backing_root = abs;
		return 0;
	}
	return 1; /* keep arg for FUSE */
}

int main(int argc, char *argv[])
{
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

	fuse_opt_parse(&args, NULL, NULL, rotfs_opt_proc);
	if (!backing_root) {
		fprintf(stderr, "rotfs: Rename On Truncate versioning filesystem\n");
		fprintf(stderr, "Usage: %s --backing=/real/root <mountpoint> [FUSE opts]\n", argv[0]);
		return 1;
	}

	int ret = fuse_main(args.argc, args.argv, &rotfs_ops, NULL);
	fuse_opt_free_args(&args);
	return ret;
}
