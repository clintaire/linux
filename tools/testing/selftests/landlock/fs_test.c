// SPDX-License-Identifier: GPL-2.0
/*
 * Landlock tests - Filesystem
 *
 * Copyright © 2017-2020 Mickaël Salaün <mic@digikod.net>
 * Copyright © 2020 ANSSI
 * Copyright © 2020-2022 Microsoft Corporation
 */

#define _GNU_SOURCE
#include <asm/termbits.h>
#include <fcntl.h>
#include <libgen.h>
#include <linux/fiemap.h>
#include <linux/landlock.h>
#include <linux/magic.h>
#include <sched.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/capability.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/un.h>
#include <sys/vfs.h>
#include <unistd.h>

/*
 * Intentionally included last to work around header conflict.
 * See https://sourceware.org/glibc/wiki/Synchronizing_Headers.
 */
#include <linux/fs.h>
#include <linux/mount.h>

/* Defines AT_EXECVE_CHECK without type conflicts. */
#define _ASM_GENERIC_FCNTL_H
#include <linux/fcntl.h>

#include "audit.h"
#include "common.h"

#ifndef renameat2
int renameat2(int olddirfd, const char *oldpath, int newdirfd,
	      const char *newpath, unsigned int flags)
{
	return syscall(__NR_renameat2, olddirfd, oldpath, newdirfd, newpath,
		       flags);
}
#endif

#ifndef open_tree
int open_tree(int dfd, const char *filename, unsigned int flags)
{
	return syscall(__NR_open_tree, dfd, filename, flags);
}
#endif

static int sys_execveat(int dirfd, const char *pathname, char *const argv[],
			char *const envp[], int flags)
{
	return syscall(__NR_execveat, dirfd, pathname, argv, envp, flags);
}

#ifndef RENAME_EXCHANGE
#define RENAME_EXCHANGE (1 << 1)
#endif

static const char bin_true[] = "./true";

/* Paths (sibling number and depth) */
static const char dir_s1d1[] = TMP_DIR "/s1d1";
static const char file1_s1d1[] = TMP_DIR "/s1d1/f1";
static const char file2_s1d1[] = TMP_DIR "/s1d1/f2";
static const char dir_s1d2[] = TMP_DIR "/s1d1/s1d2";
static const char file1_s1d2[] = TMP_DIR "/s1d1/s1d2/f1";
static const char file2_s1d2[] = TMP_DIR "/s1d1/s1d2/f2";
static const char dir_s1d3[] = TMP_DIR "/s1d1/s1d2/s1d3";
static const char file1_s1d3[] = TMP_DIR "/s1d1/s1d2/s1d3/f1";
static const char file2_s1d3[] = TMP_DIR "/s1d1/s1d2/s1d3/f2";

static const char dir_s2d1[] = TMP_DIR "/s2d1";
static const char file1_s2d1[] = TMP_DIR "/s2d1/f1";
static const char dir_s2d2[] = TMP_DIR "/s2d1/s2d2";
static const char file1_s2d2[] = TMP_DIR "/s2d1/s2d2/f1";
static const char dir_s2d3[] = TMP_DIR "/s2d1/s2d2/s2d3";
static const char file1_s2d3[] = TMP_DIR "/s2d1/s2d2/s2d3/f1";
static const char file2_s2d3[] = TMP_DIR "/s2d1/s2d2/s2d3/f2";

static const char dir_s3d1[] = TMP_DIR "/s3d1";
static const char file1_s3d1[] = TMP_DIR "/s3d1/f1";
/* dir_s3d2 is a mount point. */
static const char dir_s3d2[] = TMP_DIR "/s3d1/s3d2";
static const char dir_s3d3[] = TMP_DIR "/s3d1/s3d2/s3d3";
static const char file1_s3d3[] = TMP_DIR "/s3d1/s3d2/s3d3/f1";
static const char dir_s3d4[] = TMP_DIR "/s3d1/s3d2/s3d4";
static const char file1_s3d4[] = TMP_DIR "/s3d1/s3d2/s3d4/f1";

/*
 * layout1 hierarchy:
 *
 * tmp
 * ├── s1d1
 * │   ├── f1
 * │   ├── f2
 * │   └── s1d2
 * │       ├── f1
 * │       ├── f2
 * │       └── s1d3
 * │           ├── f1
 * │           └── f2
 * ├── s2d1
 * │   ├── f1
 * │   └── s2d2
 * │       ├── f1
 * │       └── s2d3
 * │           ├── f1
 * │           └── f2
 * └── s3d1
 *     ├── f1
 *     └── s3d2 [mount point]
 *         ├── s3d3
 *         │   └── f1
 *         └── s3d4
 *             └── f1
 */

static bool fgrep(FILE *const inf, const char *const str)
{
	char line[32];
	const int slen = strlen(str);

	while (!feof(inf)) {
		if (!fgets(line, sizeof(line), inf))
			break;
		if (strncmp(line, str, slen))
			continue;

		return true;
	}

	return false;
}

static bool supports_filesystem(const char *const filesystem)
{
	char str[32];
	int len;
	bool res = true;
	FILE *const inf = fopen("/proc/filesystems", "r");

	/*
	 * Consider that the filesystem is supported if we cannot get the
	 * supported ones.
	 */
	if (!inf)
		return true;

	/* filesystem can be null for bind mounts. */
	if (!filesystem)
		goto out;

	len = snprintf(str, sizeof(str), "nodev\t%s\n", filesystem);
	if (len >= sizeof(str))
		/* Ignores too-long filesystem names. */
		goto out;

	res = fgrep(inf, str);

out:
	fclose(inf);
	return res;
}

static bool cwd_matches_fs(unsigned int fs_magic)
{
	struct statfs statfs_buf;

	if (!fs_magic)
		return true;

	if (statfs(".", &statfs_buf))
		return true;

	return statfs_buf.f_type == fs_magic;
}

static void mkdir_parents(struct __test_metadata *const _metadata,
			  const char *const path)
{
	char *walker;
	const char *parent;
	int i, err;

	ASSERT_NE(path[0], '\0');
	walker = strdup(path);
	ASSERT_NE(NULL, walker);
	parent = walker;
	for (i = 1; walker[i]; i++) {
		if (walker[i] != '/')
			continue;
		walker[i] = '\0';
		err = mkdir(parent, 0700);
		ASSERT_FALSE(err && errno != EEXIST)
		{
			TH_LOG("Failed to create directory \"%s\": %s", parent,
			       strerror(errno));
		}
		walker[i] = '/';
	}
	free(walker);
}

static void create_directory(struct __test_metadata *const _metadata,
			     const char *const path)
{
	mkdir_parents(_metadata, path);
	ASSERT_EQ(0, mkdir(path, 0700))
	{
		TH_LOG("Failed to create directory \"%s\": %s", path,
		       strerror(errno));
	}
}

static void create_file(struct __test_metadata *const _metadata,
			const char *const path)
{
	mkdir_parents(_metadata, path);
	ASSERT_EQ(0, mknod(path, S_IFREG | 0700, 0))
	{
		TH_LOG("Failed to create file \"%s\": %s", path,
		       strerror(errno));
	}
}

static int remove_path(const char *const path)
{
	char *walker;
	int i, ret, err = 0;

	walker = strdup(path);
	if (!walker) {
		err = ENOMEM;
		goto out;
	}
	if (unlink(path) && rmdir(path)) {
		if (errno != ENOENT && errno != ENOTDIR)
			err = errno;
		goto out;
	}
	for (i = strlen(walker); i > 0; i--) {
		if (walker[i] != '/')
			continue;
		walker[i] = '\0';
		ret = rmdir(walker);
		if (ret) {
			if (errno != ENOTEMPTY && errno != EBUSY)
				err = errno;
			goto out;
		}
		if (strcmp(walker, TMP_DIR) == 0)
			goto out;
	}

out:
	free(walker);
	return err;
}

struct mnt_opt {
	const char *const source;
	const char *const type;
	const unsigned long flags;
	const char *const data;
};

#define MNT_TMP_DATA "size=4m,mode=700"

static const struct mnt_opt mnt_tmp = {
	.type = "tmpfs",
	.data = MNT_TMP_DATA,
};

static int mount_opt(const struct mnt_opt *const mnt, const char *const target)
{
	return mount(mnt->source ?: mnt->type, target, mnt->type, mnt->flags,
		     mnt->data);
}

static void prepare_layout_opt(struct __test_metadata *const _metadata,
			       const struct mnt_opt *const mnt)
{
	disable_caps(_metadata);
	umask(0077);
	create_directory(_metadata, TMP_DIR);

	/*
	 * Do not pollute the rest of the system: creates a private mount point
	 * for tests relying on pivot_root(2) and move_mount(2).
	 */
	set_cap(_metadata, CAP_SYS_ADMIN);
	ASSERT_EQ(0, unshare(CLONE_NEWNS | CLONE_NEWCGROUP));
	ASSERT_EQ(0, mount_opt(mnt, TMP_DIR))
	{
		TH_LOG("Failed to mount the %s filesystem: %s", mnt->type,
		       strerror(errno));
		/*
		 * FIXTURE_TEARDOWN() is not called when FIXTURE_SETUP()
		 * failed, so we need to explicitly do a minimal cleanup to
		 * avoid cascading errors with other tests that don't depend on
		 * the same filesystem.
		 */
		remove_path(TMP_DIR);
	}
	ASSERT_EQ(0, mount(NULL, TMP_DIR, NULL, MS_PRIVATE | MS_REC, NULL));
	clear_cap(_metadata, CAP_SYS_ADMIN);
}

static void prepare_layout(struct __test_metadata *const _metadata)
{
	prepare_layout_opt(_metadata, &mnt_tmp);
}

static void cleanup_layout(struct __test_metadata *const _metadata)
{
	set_cap(_metadata, CAP_SYS_ADMIN);
	if (umount(TMP_DIR)) {
		/*
		 * According to the test environment, the mount point of the
		 * current directory may be shared or not, which changes the
		 * visibility of the nested TMP_DIR mount point for the test's
		 * parent process doing this cleanup.
		 */
		ASSERT_EQ(EINVAL, errno);
	}
	clear_cap(_metadata, CAP_SYS_ADMIN);
	EXPECT_EQ(0, remove_path(TMP_DIR));
}

/* clang-format off */
FIXTURE(layout0) {};
/* clang-format on */

FIXTURE_SETUP(layout0)
{
	prepare_layout(_metadata);
}

FIXTURE_TEARDOWN_PARENT(layout0)
{
	cleanup_layout(_metadata);
}

static void create_layout1(struct __test_metadata *const _metadata)
{
	create_file(_metadata, file1_s1d1);
	create_file(_metadata, file1_s1d2);
	create_file(_metadata, file1_s1d3);
	create_file(_metadata, file2_s1d1);
	create_file(_metadata, file2_s1d2);
	create_file(_metadata, file2_s1d3);

	create_file(_metadata, file1_s2d1);
	create_file(_metadata, file1_s2d2);
	create_file(_metadata, file1_s2d3);
	create_file(_metadata, file2_s2d3);

	create_file(_metadata, file1_s3d1);
	create_directory(_metadata, dir_s3d2);
	set_cap(_metadata, CAP_SYS_ADMIN);
	ASSERT_EQ(0, mount_opt(&mnt_tmp, dir_s3d2));
	clear_cap(_metadata, CAP_SYS_ADMIN);

	create_file(_metadata, file1_s3d3);
	create_file(_metadata, file1_s3d4);
}

static void remove_layout1(struct __test_metadata *const _metadata)
{
	EXPECT_EQ(0, remove_path(file2_s1d3));
	EXPECT_EQ(0, remove_path(file2_s1d2));
	EXPECT_EQ(0, remove_path(file2_s1d1));
	EXPECT_EQ(0, remove_path(file1_s1d3));
	EXPECT_EQ(0, remove_path(file1_s1d2));
	EXPECT_EQ(0, remove_path(file1_s1d1));
	EXPECT_EQ(0, remove_path(dir_s1d3));

	EXPECT_EQ(0, remove_path(file2_s2d3));
	EXPECT_EQ(0, remove_path(file1_s2d3));
	EXPECT_EQ(0, remove_path(file1_s2d2));
	EXPECT_EQ(0, remove_path(file1_s2d1));
	EXPECT_EQ(0, remove_path(dir_s2d2));

	EXPECT_EQ(0, remove_path(file1_s3d1));
	EXPECT_EQ(0, remove_path(file1_s3d3));
	EXPECT_EQ(0, remove_path(file1_s3d4));
	set_cap(_metadata, CAP_SYS_ADMIN);
	umount(dir_s3d2);
	clear_cap(_metadata, CAP_SYS_ADMIN);
	EXPECT_EQ(0, remove_path(dir_s3d2));
}

/* clang-format off */
FIXTURE(layout1) {};
/* clang-format on */

FIXTURE_SETUP(layout1)
{
	prepare_layout(_metadata);

	create_layout1(_metadata);
}

FIXTURE_TEARDOWN_PARENT(layout1)
{
	remove_layout1(_metadata);

	cleanup_layout(_metadata);
}

/*
 * This helper enables to use the ASSERT_* macros and print the line number
 * pointing to the test caller.
 */
static int test_open_rel(const int dirfd, const char *const path,
			 const int flags)
{
	int fd;

	/* Works with file and directories. */
	fd = openat(dirfd, path, flags | O_CLOEXEC);
	if (fd < 0)
		return errno;
	/*
	 * Mixing error codes from close(2) and open(2) should not lead to any
	 * (access type) confusion for this test.
	 */
	if (close(fd) != 0)
		return errno;
	return 0;
}

static int test_open(const char *const path, const int flags)
{
	return test_open_rel(AT_FDCWD, path, flags);
}

TEST_F_FORK(layout1, no_restriction)
{
	ASSERT_EQ(0, test_open(dir_s1d1, O_RDONLY));
	ASSERT_EQ(0, test_open(file1_s1d1, O_RDONLY));
	ASSERT_EQ(0, test_open(file2_s1d1, O_RDONLY));
	ASSERT_EQ(0, test_open(dir_s1d2, O_RDONLY));
	ASSERT_EQ(0, test_open(file1_s1d2, O_RDONLY));
	ASSERT_EQ(0, test_open(file2_s1d2, O_RDONLY));
	ASSERT_EQ(0, test_open(dir_s1d3, O_RDONLY));
	ASSERT_EQ(0, test_open(file1_s1d3, O_RDONLY));

	ASSERT_EQ(0, test_open(dir_s2d1, O_RDONLY));
	ASSERT_EQ(0, test_open(file1_s2d1, O_RDONLY));
	ASSERT_EQ(0, test_open(dir_s2d2, O_RDONLY));
	ASSERT_EQ(0, test_open(file1_s2d2, O_RDONLY));
	ASSERT_EQ(0, test_open(dir_s2d3, O_RDONLY));
	ASSERT_EQ(0, test_open(file1_s2d3, O_RDONLY));

	ASSERT_EQ(0, test_open(dir_s3d1, O_RDONLY));
	ASSERT_EQ(0, test_open(dir_s3d2, O_RDONLY));
	ASSERT_EQ(0, test_open(dir_s3d3, O_RDONLY));
}

TEST_F_FORK(layout1, inval)
{
	struct landlock_path_beneath_attr path_beneath = {
		.allowed_access = LANDLOCK_ACCESS_FS_READ_FILE |
				  LANDLOCK_ACCESS_FS_WRITE_FILE,
		.parent_fd = -1,
	};
	struct landlock_ruleset_attr ruleset_attr = {
		.handled_access_fs = LANDLOCK_ACCESS_FS_READ_FILE |
				     LANDLOCK_ACCESS_FS_WRITE_FILE,
	};
	int ruleset_fd;

	path_beneath.parent_fd =
		open(dir_s1d2, O_PATH | O_DIRECTORY | O_CLOEXEC);
	ASSERT_LE(0, path_beneath.parent_fd);

	ruleset_fd = open(dir_s1d1, O_PATH | O_DIRECTORY | O_CLOEXEC);
	ASSERT_LE(0, ruleset_fd);
	ASSERT_EQ(-1, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_PATH_BENEATH,
					&path_beneath, 0));
	/* Returns EBADF because ruleset_fd is not a landlock-ruleset FD. */
	ASSERT_EQ(EBADF, errno);
	ASSERT_EQ(0, close(ruleset_fd));

	ruleset_fd = open(dir_s1d1, O_DIRECTORY | O_CLOEXEC);
	ASSERT_LE(0, ruleset_fd);
	ASSERT_EQ(-1, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_PATH_BENEATH,
					&path_beneath, 0));
	/* Returns EBADFD because ruleset_fd is not a valid ruleset. */
	ASSERT_EQ(EBADFD, errno);
	ASSERT_EQ(0, close(ruleset_fd));

	/* Gets a real ruleset. */
	ruleset_fd =
		landlock_create_ruleset(&ruleset_attr, sizeof(ruleset_attr), 0);
	ASSERT_LE(0, ruleset_fd);
	ASSERT_EQ(0, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_PATH_BENEATH,
				       &path_beneath, 0));
	ASSERT_EQ(0, close(path_beneath.parent_fd));

	/* Tests without O_PATH. */
	path_beneath.parent_fd = open(dir_s1d2, O_DIRECTORY | O_CLOEXEC);
	ASSERT_LE(0, path_beneath.parent_fd);
	ASSERT_EQ(0, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_PATH_BENEATH,
				       &path_beneath, 0));
	ASSERT_EQ(0, close(path_beneath.parent_fd));

	/* Tests with a ruleset FD. */
	path_beneath.parent_fd = ruleset_fd;
	ASSERT_EQ(-1, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_PATH_BENEATH,
					&path_beneath, 0));
	ASSERT_EQ(EBADFD, errno);

	/* Checks unhandled allowed_access. */
	path_beneath.parent_fd =
		open(dir_s1d2, O_PATH | O_DIRECTORY | O_CLOEXEC);
	ASSERT_LE(0, path_beneath.parent_fd);

	/* Test with legitimate values. */
	path_beneath.allowed_access |= LANDLOCK_ACCESS_FS_EXECUTE;
	ASSERT_EQ(-1, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_PATH_BENEATH,
					&path_beneath, 0));
	ASSERT_EQ(EINVAL, errno);
	path_beneath.allowed_access &= ~LANDLOCK_ACCESS_FS_EXECUTE;

	/* Tests with denied-by-default access right. */
	path_beneath.allowed_access |= LANDLOCK_ACCESS_FS_REFER;
	ASSERT_EQ(-1, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_PATH_BENEATH,
					&path_beneath, 0));
	ASSERT_EQ(EINVAL, errno);
	path_beneath.allowed_access &= ~LANDLOCK_ACCESS_FS_REFER;

	/* Test with unknown (64-bits) value. */
	path_beneath.allowed_access |= (1ULL << 60);
	ASSERT_EQ(-1, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_PATH_BENEATH,
					&path_beneath, 0));
	ASSERT_EQ(EINVAL, errno);
	path_beneath.allowed_access &= ~(1ULL << 60);

	/* Test with no access. */
	path_beneath.allowed_access = 0;
	ASSERT_EQ(-1, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_PATH_BENEATH,
					&path_beneath, 0));
	ASSERT_EQ(ENOMSG, errno);
	path_beneath.allowed_access &= ~(1ULL << 60);

	ASSERT_EQ(0, close(path_beneath.parent_fd));

	/* Enforces the ruleset. */
	ASSERT_EQ(0, prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0));
	ASSERT_EQ(0, landlock_restrict_self(ruleset_fd, 0));

	ASSERT_EQ(0, close(ruleset_fd));
}

/* clang-format off */

#define ACCESS_FILE ( \
	LANDLOCK_ACCESS_FS_EXECUTE | \
	LANDLOCK_ACCESS_FS_WRITE_FILE | \
	LANDLOCK_ACCESS_FS_READ_FILE | \
	LANDLOCK_ACCESS_FS_TRUNCATE | \
	LANDLOCK_ACCESS_FS_IOCTL_DEV)

#define ACCESS_LAST LANDLOCK_ACCESS_FS_IOCTL_DEV

#define ACCESS_ALL ( \
	ACCESS_FILE | \
	LANDLOCK_ACCESS_FS_READ_DIR | \
	LANDLOCK_ACCESS_FS_REMOVE_DIR | \
	LANDLOCK_ACCESS_FS_REMOVE_FILE | \
	LANDLOCK_ACCESS_FS_MAKE_CHAR | \
	LANDLOCK_ACCESS_FS_MAKE_DIR | \
	LANDLOCK_ACCESS_FS_MAKE_REG | \
	LANDLOCK_ACCESS_FS_MAKE_SOCK | \
	LANDLOCK_ACCESS_FS_MAKE_FIFO | \
	LANDLOCK_ACCESS_FS_MAKE_BLOCK | \
	LANDLOCK_ACCESS_FS_MAKE_SYM | \
	LANDLOCK_ACCESS_FS_REFER)

/* clang-format on */

TEST_F_FORK(layout1, file_and_dir_access_rights)
{
	__u64 access;
	int err;
	struct landlock_path_beneath_attr path_beneath_file = {},
					  path_beneath_dir = {};
	struct landlock_ruleset_attr ruleset_attr = {
		.handled_access_fs = ACCESS_ALL,
	};
	const int ruleset_fd =
		landlock_create_ruleset(&ruleset_attr, sizeof(ruleset_attr), 0);

	ASSERT_LE(0, ruleset_fd);

	/* Tests access rights for files. */
	path_beneath_file.parent_fd = open(file1_s1d2, O_PATH | O_CLOEXEC);
	ASSERT_LE(0, path_beneath_file.parent_fd);

	/* Tests access rights for directories. */
	path_beneath_dir.parent_fd =
		open(dir_s1d2, O_PATH | O_DIRECTORY | O_CLOEXEC);
	ASSERT_LE(0, path_beneath_dir.parent_fd);

	for (access = 1; access <= ACCESS_LAST; access <<= 1) {
		path_beneath_dir.allowed_access = access;
		ASSERT_EQ(0, landlock_add_rule(ruleset_fd,
					       LANDLOCK_RULE_PATH_BENEATH,
					       &path_beneath_dir, 0));

		path_beneath_file.allowed_access = access;
		err = landlock_add_rule(ruleset_fd, LANDLOCK_RULE_PATH_BENEATH,
					&path_beneath_file, 0);
		if (access & ACCESS_FILE) {
			ASSERT_EQ(0, err);
		} else {
			ASSERT_EQ(-1, err);
			ASSERT_EQ(EINVAL, errno);
		}
	}
	ASSERT_EQ(0, close(path_beneath_file.parent_fd));
	ASSERT_EQ(0, close(path_beneath_dir.parent_fd));
	ASSERT_EQ(0, close(ruleset_fd));
}

TEST_F_FORK(layout0, ruleset_with_unknown_access)
{
	__u64 access_mask;

	for (access_mask = 1ULL << 63; access_mask != ACCESS_LAST;
	     access_mask >>= 1) {
		struct landlock_ruleset_attr ruleset_attr = {
			.handled_access_fs = access_mask,
		};

		ASSERT_EQ(-1, landlock_create_ruleset(&ruleset_attr,
						      sizeof(ruleset_attr), 0));
		ASSERT_EQ(EINVAL, errno);
	}
}

TEST_F_FORK(layout0, rule_with_unknown_access)
{
	__u64 access;
	struct landlock_path_beneath_attr path_beneath = {};
	const struct landlock_ruleset_attr ruleset_attr = {
		.handled_access_fs = ACCESS_ALL,
	};
	const int ruleset_fd =
		landlock_create_ruleset(&ruleset_attr, sizeof(ruleset_attr), 0);

	ASSERT_LE(0, ruleset_fd);

	path_beneath.parent_fd =
		open(TMP_DIR, O_PATH | O_DIRECTORY | O_CLOEXEC);
	ASSERT_LE(0, path_beneath.parent_fd);

	for (access = 1ULL << 63; access != ACCESS_LAST; access >>= 1) {
		path_beneath.allowed_access = access;
		EXPECT_EQ(-1, landlock_add_rule(ruleset_fd,
						LANDLOCK_RULE_PATH_BENEATH,
						&path_beneath, 0));
		EXPECT_EQ(EINVAL, errno);
	}
	ASSERT_EQ(0, close(path_beneath.parent_fd));
	ASSERT_EQ(0, close(ruleset_fd));
}

TEST_F_FORK(layout1, rule_with_unhandled_access)
{
	struct landlock_ruleset_attr ruleset_attr = {
		.handled_access_fs = LANDLOCK_ACCESS_FS_EXECUTE,
	};
	struct landlock_path_beneath_attr path_beneath = {};
	int ruleset_fd;
	__u64 access;

	ruleset_fd =
		landlock_create_ruleset(&ruleset_attr, sizeof(ruleset_attr), 0);
	ASSERT_LE(0, ruleset_fd);

	path_beneath.parent_fd = open(file1_s1d2, O_PATH | O_CLOEXEC);
	ASSERT_LE(0, path_beneath.parent_fd);

	for (access = 1; access > 0; access <<= 1) {
		int err;

		path_beneath.allowed_access = access;
		err = landlock_add_rule(ruleset_fd, LANDLOCK_RULE_PATH_BENEATH,
					&path_beneath, 0);
		if (access == ruleset_attr.handled_access_fs) {
			EXPECT_EQ(0, err);
		} else {
			EXPECT_EQ(-1, err);
			EXPECT_EQ(EINVAL, errno);
		}
	}

	EXPECT_EQ(0, close(path_beneath.parent_fd));
	EXPECT_EQ(0, close(ruleset_fd));
}

static void add_path_beneath(struct __test_metadata *const _metadata,
			     const int ruleset_fd, const __u64 allowed_access,
			     const char *const path)
{
	struct landlock_path_beneath_attr path_beneath = {
		.allowed_access = allowed_access,
	};

	path_beneath.parent_fd = open(path, O_PATH | O_CLOEXEC);
	ASSERT_LE(0, path_beneath.parent_fd)
	{
		TH_LOG("Failed to open directory \"%s\": %s", path,
		       strerror(errno));
	}
	ASSERT_EQ(0, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_PATH_BENEATH,
				       &path_beneath, 0))
	{
		TH_LOG("Failed to update the ruleset with \"%s\": %s", path,
		       strerror(errno));
	}
	ASSERT_EQ(0, close(path_beneath.parent_fd));
}

struct rule {
	const char *path;
	__u64 access;
};

/* clang-format off */

#define ACCESS_RO ( \
	LANDLOCK_ACCESS_FS_READ_FILE | \
	LANDLOCK_ACCESS_FS_READ_DIR)

#define ACCESS_RW ( \
	ACCESS_RO | \
	LANDLOCK_ACCESS_FS_WRITE_FILE)

/* clang-format on */

static int create_ruleset(struct __test_metadata *const _metadata,
			  const __u64 handled_access_fs,
			  const struct rule rules[])
{
	int ruleset_fd, i;
	struct landlock_ruleset_attr ruleset_attr = {
		.handled_access_fs = handled_access_fs,
	};

	ASSERT_NE(NULL, rules)
	{
		TH_LOG("No rule list");
	}
	ASSERT_NE(NULL, rules[0].path)
	{
		TH_LOG("Empty rule list");
	}

	ruleset_fd =
		landlock_create_ruleset(&ruleset_attr, sizeof(ruleset_attr), 0);
	ASSERT_LE(0, ruleset_fd)
	{
		TH_LOG("Failed to create a ruleset: %s", strerror(errno));
	}

	for (i = 0; rules[i].path; i++) {
		if (!rules[i].access)
			continue;

		add_path_beneath(_metadata, ruleset_fd, rules[i].access,
				 rules[i].path);
	}
	return ruleset_fd;
}

TEST_F_FORK(layout0, proc_nsfs)
{
	const struct rule rules[] = {
		{
			.path = "/dev/null",
			.access = LANDLOCK_ACCESS_FS_READ_FILE |
				  LANDLOCK_ACCESS_FS_WRITE_FILE,
		},
		{},
	};
	struct landlock_path_beneath_attr path_beneath;
	const int ruleset_fd = create_ruleset(
		_metadata, rules[0].access | LANDLOCK_ACCESS_FS_READ_DIR,
		rules);

	ASSERT_LE(0, ruleset_fd);
	ASSERT_EQ(0, test_open("/proc/self/ns/mnt", O_RDONLY));

	enforce_ruleset(_metadata, ruleset_fd);

	ASSERT_EQ(EACCES, test_open("/", O_RDONLY));
	ASSERT_EQ(EACCES, test_open("/dev", O_RDONLY));
	ASSERT_EQ(0, test_open("/dev/null", O_RDONLY));
	ASSERT_EQ(EACCES, test_open("/dev/full", O_RDONLY));

	ASSERT_EQ(EACCES, test_open("/proc", O_RDONLY));
	ASSERT_EQ(EACCES, test_open("/proc/self", O_RDONLY));
	ASSERT_EQ(EACCES, test_open("/proc/self/ns", O_RDONLY));
	/*
	 * Because nsfs is an internal filesystem, /proc/self/ns/mnt is a
	 * disconnected path.  Such path cannot be identified and must then be
	 * allowed.
	 */
	ASSERT_EQ(0, test_open("/proc/self/ns/mnt", O_RDONLY));

	/*
	 * Checks that it is not possible to add nsfs-like filesystem
	 * references to a ruleset.
	 */
	path_beneath.allowed_access = LANDLOCK_ACCESS_FS_READ_FILE |
				      LANDLOCK_ACCESS_FS_WRITE_FILE,
	path_beneath.parent_fd = open("/proc/self/ns/mnt", O_PATH | O_CLOEXEC);
	ASSERT_LE(0, path_beneath.parent_fd);
	ASSERT_EQ(-1, landlock_add_rule(ruleset_fd, LANDLOCK_RULE_PATH_BENEATH,
					&path_beneath, 0));
	ASSERT_EQ(EBADFD, errno);
	ASSERT_EQ(0, close(path_beneath.parent_fd));
}

TEST_F_FORK(layout0, unpriv)
{
	const struct rule rules[] = {
		{
			.path = TMP_DIR,
			.access = ACCESS_RO,
		},
		{},
	};
	int ruleset_fd;

	drop_caps(_metadata);

	ruleset_fd = create_ruleset(_metadata, ACCESS_RO, rules);
	ASSERT_LE(0, ruleset_fd);
	ASSERT_EQ(-1, landlock_restrict_self(ruleset_fd, 0));
	ASSERT_EQ(EPERM, errno);

	/* enforce_ruleset() calls prctl(no_new_privs). */
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));
}

TEST_F_FORK(layout1, effective_access)
{
	const struct rule rules[] = {
		{
			.path = dir_s1d2,
			.access = ACCESS_RO,
		},
		{
			.path = file1_s2d2,
			.access = LANDLOCK_ACCESS_FS_READ_FILE |
				  LANDLOCK_ACCESS_FS_WRITE_FILE,
		},
		{},
	};
	const int ruleset_fd = create_ruleset(_metadata, ACCESS_RW, rules);
	char buf;
	int reg_fd;

	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/* Tests on a directory (with or without O_PATH). */
	ASSERT_EQ(EACCES, test_open("/", O_RDONLY));
	ASSERT_EQ(0, test_open("/", O_RDONLY | O_PATH));
	ASSERT_EQ(EACCES, test_open(dir_s1d1, O_RDONLY));
	ASSERT_EQ(0, test_open(dir_s1d1, O_RDONLY | O_PATH));
	ASSERT_EQ(EACCES, test_open(file1_s1d1, O_RDONLY));
	ASSERT_EQ(0, test_open(file1_s1d1, O_RDONLY | O_PATH));

	ASSERT_EQ(0, test_open(dir_s1d2, O_RDONLY));
	ASSERT_EQ(0, test_open(file1_s1d2, O_RDONLY));
	ASSERT_EQ(0, test_open(dir_s1d3, O_RDONLY));
	ASSERT_EQ(0, test_open(file1_s1d3, O_RDONLY));

	/* Tests on a file (with or without O_PATH). */
	ASSERT_EQ(EACCES, test_open(dir_s2d2, O_RDONLY));
	ASSERT_EQ(0, test_open(dir_s2d2, O_RDONLY | O_PATH));

	ASSERT_EQ(0, test_open(file1_s2d2, O_RDONLY));

	/* Checks effective read and write actions. */
	reg_fd = open(file1_s2d2, O_RDWR | O_CLOEXEC);
	ASSERT_LE(0, reg_fd);
	ASSERT_EQ(1, write(reg_fd, ".", 1));
	ASSERT_LE(0, lseek(reg_fd, 0, SEEK_SET));
	ASSERT_EQ(1, read(reg_fd, &buf, 1));
	ASSERT_EQ('.', buf);
	ASSERT_EQ(0, close(reg_fd));

	/* Just in case, double-checks effective actions. */
	reg_fd = open(file1_s2d2, O_RDONLY | O_CLOEXEC);
	ASSERT_LE(0, reg_fd);
	ASSERT_EQ(-1, write(reg_fd, &buf, 1));
	ASSERT_EQ(EBADF, errno);
	ASSERT_EQ(0, close(reg_fd));
}

TEST_F_FORK(layout1, unhandled_access)
{
	const struct rule rules[] = {
		{
			.path = dir_s1d2,
			.access = ACCESS_RO,
		},
		{},
	};
	/* Here, we only handle read accesses, not write accesses. */
	const int ruleset_fd = create_ruleset(_metadata, ACCESS_RO, rules);

	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/*
	 * Because the policy does not handle LANDLOCK_ACCESS_FS_WRITE_FILE,
	 * opening for write-only should be allowed, but not read-write.
	 */
	ASSERT_EQ(0, test_open(file1_s1d1, O_WRONLY));
	ASSERT_EQ(EACCES, test_open(file1_s1d1, O_RDWR));

	ASSERT_EQ(0, test_open(file1_s1d2, O_WRONLY));
	ASSERT_EQ(0, test_open(file1_s1d2, O_RDWR));
}

TEST_F_FORK(layout1, ruleset_overlap)
{
	const struct rule rules[] = {
		/* These rules should be ORed among them. */
		{
			.path = dir_s1d2,
			.access = LANDLOCK_ACCESS_FS_READ_FILE |
				  LANDLOCK_ACCESS_FS_WRITE_FILE,
		},
		{
			.path = dir_s1d2,
			.access = LANDLOCK_ACCESS_FS_READ_FILE |
				  LANDLOCK_ACCESS_FS_READ_DIR,
		},
		{},
	};
	const int ruleset_fd = create_ruleset(_metadata, ACCESS_RW, rules);

	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/* Checks s1d1 hierarchy. */
	ASSERT_EQ(EACCES, test_open(file1_s1d1, O_RDONLY));
	ASSERT_EQ(EACCES, test_open(file1_s1d1, O_WRONLY));
	ASSERT_EQ(EACCES, test_open(file1_s1d1, O_RDWR));
	ASSERT_EQ(EACCES, test_open(dir_s1d1, O_RDONLY | O_DIRECTORY));

	/* Checks s1d2 hierarchy. */
	ASSERT_EQ(0, test_open(file1_s1d2, O_RDONLY));
	ASSERT_EQ(0, test_open(file1_s1d2, O_WRONLY));
	ASSERT_EQ(0, test_open(file1_s1d2, O_RDWR));
	ASSERT_EQ(0, test_open(dir_s1d2, O_RDONLY | O_DIRECTORY));

	/* Checks s1d3 hierarchy. */
	ASSERT_EQ(0, test_open(file1_s1d3, O_RDONLY));
	ASSERT_EQ(0, test_open(file1_s1d3, O_WRONLY));
	ASSERT_EQ(0, test_open(file1_s1d3, O_RDWR));
	ASSERT_EQ(0, test_open(dir_s1d3, O_RDONLY | O_DIRECTORY));
}

TEST_F_FORK(layout1, layer_rule_unions)
{
	const struct rule layer1[] = {
		{
			.path = dir_s1d2,
			.access = LANDLOCK_ACCESS_FS_READ_FILE,
		},
		/* dir_s1d3 should allow READ_FILE and WRITE_FILE (O_RDWR). */
		{
			.path = dir_s1d3,
			.access = LANDLOCK_ACCESS_FS_WRITE_FILE,
		},
		{},
	};
	const struct rule layer2[] = {
		/* Doesn't change anything from layer1. */
		{
			.path = dir_s1d2,
			.access = LANDLOCK_ACCESS_FS_READ_FILE |
				  LANDLOCK_ACCESS_FS_WRITE_FILE,
		},
		{},
	};
	const struct rule layer3[] = {
		/* Only allows write (but not read) to dir_s1d3. */
		{
			.path = dir_s1d2,
			.access = LANDLOCK_ACCESS_FS_WRITE_FILE,
		},
		{},
	};
	int ruleset_fd = create_ruleset(_metadata, ACCESS_RW, layer1);

	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/* Checks s1d1 hierarchy with layer1. */
	ASSERT_EQ(EACCES, test_open(file1_s1d1, O_RDONLY));
	ASSERT_EQ(EACCES, test_open(file1_s1d1, O_WRONLY));
	ASSERT_EQ(EACCES, test_open(file1_s1d1, O_RDWR));
	ASSERT_EQ(EACCES, test_open(dir_s1d1, O_RDONLY | O_DIRECTORY));

	/* Checks s1d2 hierarchy with layer1. */
	ASSERT_EQ(0, test_open(file1_s1d2, O_RDONLY));
	ASSERT_EQ(EACCES, test_open(file1_s1d2, O_WRONLY));
	ASSERT_EQ(EACCES, test_open(file1_s1d2, O_RDWR));
	ASSERT_EQ(EACCES, test_open(dir_s1d1, O_RDONLY | O_DIRECTORY));

	/* Checks s1d3 hierarchy with layer1. */
	ASSERT_EQ(0, test_open(file1_s1d3, O_RDONLY));
	ASSERT_EQ(0, test_open(file1_s1d3, O_WRONLY));
	/* dir_s1d3 should allow READ_FILE and WRITE_FILE (O_RDWR). */
	ASSERT_EQ(0, test_open(file1_s1d3, O_RDWR));
	ASSERT_EQ(EACCES, test_open(dir_s1d1, O_RDONLY | O_DIRECTORY));

	/* Doesn't change anything from layer1. */
	ruleset_fd = create_ruleset(_metadata, ACCESS_RW, layer2);
	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/* Checks s1d1 hierarchy with layer2. */
	ASSERT_EQ(EACCES, test_open(file1_s1d1, O_RDONLY));
	ASSERT_EQ(EACCES, test_open(file1_s1d1, O_WRONLY));
	ASSERT_EQ(EACCES, test_open(file1_s1d1, O_RDWR));
	ASSERT_EQ(EACCES, test_open(dir_s1d1, O_RDONLY | O_DIRECTORY));

	/* Checks s1d2 hierarchy with layer2. */
	ASSERT_EQ(0, test_open(file1_s1d2, O_RDONLY));
	ASSERT_EQ(EACCES, test_open(file1_s1d2, O_WRONLY));
	ASSERT_EQ(EACCES, test_open(file1_s1d2, O_RDWR));
	ASSERT_EQ(EACCES, test_open(dir_s1d1, O_RDONLY | O_DIRECTORY));

	/* Checks s1d3 hierarchy with layer2. */
	ASSERT_EQ(0, test_open(file1_s1d3, O_RDONLY));
	ASSERT_EQ(0, test_open(file1_s1d3, O_WRONLY));
	/* dir_s1d3 should allow READ_FILE and WRITE_FILE (O_RDWR). */
	ASSERT_EQ(0, test_open(file1_s1d3, O_RDWR));
	ASSERT_EQ(EACCES, test_open(dir_s1d1, O_RDONLY | O_DIRECTORY));

	/* Only allows write (but not read) to dir_s1d3. */
	ruleset_fd = create_ruleset(_metadata, ACCESS_RW, layer3);
	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/* Checks s1d1 hierarchy with layer3. */
	ASSERT_EQ(EACCES, test_open(file1_s1d1, O_RDONLY));
	ASSERT_EQ(EACCES, test_open(file1_s1d1, O_WRONLY));
	ASSERT_EQ(EACCES, test_open(file1_s1d1, O_RDWR));
	ASSERT_EQ(EACCES, test_open(dir_s1d1, O_RDONLY | O_DIRECTORY));

	/* Checks s1d2 hierarchy with layer3. */
	ASSERT_EQ(EACCES, test_open(file1_s1d2, O_RDONLY));
	ASSERT_EQ(EACCES, test_open(file1_s1d2, O_WRONLY));
	ASSERT_EQ(EACCES, test_open(file1_s1d2, O_RDWR));
	ASSERT_EQ(EACCES, test_open(dir_s1d1, O_RDONLY | O_DIRECTORY));

	/* Checks s1d3 hierarchy with layer3. */
	ASSERT_EQ(EACCES, test_open(file1_s1d3, O_RDONLY));
	ASSERT_EQ(0, test_open(file1_s1d3, O_WRONLY));
	/* dir_s1d3 should now deny READ_FILE and WRITE_FILE (O_RDWR). */
	ASSERT_EQ(EACCES, test_open(file1_s1d3, O_RDWR));
	ASSERT_EQ(EACCES, test_open(dir_s1d1, O_RDONLY | O_DIRECTORY));
}

TEST_F_FORK(layout1, non_overlapping_accesses)
{
	const struct rule layer1[] = {
		{
			.path = dir_s1d2,
			.access = LANDLOCK_ACCESS_FS_MAKE_REG,
		},
		{},
	};
	const struct rule layer2[] = {
		{
			.path = dir_s1d3,
			.access = LANDLOCK_ACCESS_FS_REMOVE_FILE,
		},
		{},
	};
	int ruleset_fd;

	ASSERT_EQ(0, unlink(file1_s1d1));
	ASSERT_EQ(0, unlink(file1_s1d2));

	ruleset_fd =
		create_ruleset(_metadata, LANDLOCK_ACCESS_FS_MAKE_REG, layer1);
	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	ASSERT_EQ(-1, mknod(file1_s1d1, S_IFREG | 0700, 0));
	ASSERT_EQ(EACCES, errno);
	ASSERT_EQ(0, mknod(file1_s1d2, S_IFREG | 0700, 0));
	ASSERT_EQ(0, unlink(file1_s1d2));

	ruleset_fd = create_ruleset(_metadata, LANDLOCK_ACCESS_FS_REMOVE_FILE,
				    layer2);
	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/* Unchanged accesses for file creation. */
	ASSERT_EQ(-1, mknod(file1_s1d1, S_IFREG | 0700, 0));
	ASSERT_EQ(EACCES, errno);
	ASSERT_EQ(0, mknod(file1_s1d2, S_IFREG | 0700, 0));

	/* Checks file removing. */
	ASSERT_EQ(-1, unlink(file1_s1d2));
	ASSERT_EQ(EACCES, errno);
	ASSERT_EQ(0, unlink(file1_s1d3));
}

TEST_F_FORK(layout1, interleaved_masked_accesses)
{
	/*
	 * Checks overly restrictive rules:
	 * layer 1: allows R   s1d1/s1d2/s1d3/file1
	 * layer 2: allows RW  s1d1/s1d2/s1d3
	 *          allows  W  s1d1/s1d2
	 *          denies R   s1d1/s1d2
	 * layer 3: allows R   s1d1
	 * layer 4: allows R   s1d1/s1d2
	 *          denies  W  s1d1/s1d2
	 * layer 5: allows R   s1d1/s1d2
	 * layer 6: allows   X ----
	 * layer 7: allows  W  s1d1/s1d2
	 *          denies R   s1d1/s1d2
	 */
	const struct rule layer1_read[] = {
		/* Allows read access to file1_s1d3 with the first layer. */
		{
			.path = file1_s1d3,
			.access = LANDLOCK_ACCESS_FS_READ_FILE,
		},
		{},
	};
	/* First rule with write restrictions. */
	const struct rule layer2_read_write[] = {
		/* Start by granting read-write access via its parent directory... */
		{
			.path = dir_s1d3,
			.access = LANDLOCK_ACCESS_FS_READ_FILE |
				  LANDLOCK_ACCESS_FS_WRITE_FILE,
		},
		/* ...but also denies read access via its grandparent directory. */
		{
			.path = dir_s1d2,
			.access = LANDLOCK_ACCESS_FS_WRITE_FILE,
		},
		{},
	};
	const struct rule layer3_read[] = {
		/* Allows read access via its great-grandparent directory. */
		{
			.path = dir_s1d1,
			.access = LANDLOCK_ACCESS_FS_READ_FILE,
		},
		{},
	};
	const struct rule layer4_read_write[] = {
		/*
		 * Try to confuse the deny access by denying write (but not
		 * read) access via its grandparent directory.
		 */
		{
			.path = dir_s1d2,
			.access = LANDLOCK_ACCESS_FS_READ_FILE,
		},
		{},
	};
	const struct rule layer5_read[] = {
		/*
		 * Try to override layer2's deny read access by explicitly
		 * allowing read access via file1_s1d3's grandparent.
		 */
		{
			.path = dir_s1d2,
			.access = LANDLOCK_ACCESS_FS_READ_FILE,
		},
		{},
	};
	const struct rule layer6_execute[] = {
		/*
		 * Restricts an unrelated file hierarchy with a new access
		 * (non-overlapping) type.
		 */
		{
			.path = dir_s2d1,
			.access = LANDLOCK_ACCESS_FS_EXECUTE,
		},
		{},
	};
	const struct rule layer7_read_write[] = {
		/*
		 * Finally, denies read access to file1_s1d3 via its
		 * grandparent.
		 */
		{
			.path = dir_s1d2,
			.access = LANDLOCK_ACCESS_FS_WRITE_FILE,
		},
		{},
	};
	int ruleset_fd;

	ruleset_fd = create_ruleset(_metadata, LANDLOCK_ACCESS_FS_READ_FILE,
				    layer1_read);
	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/* Checks that read access is granted for file1_s1d3 with layer 1. */
	ASSERT_EQ(0, test_open(file1_s1d3, O_RDWR));
	ASSERT_EQ(EACCES, test_open(file2_s1d3, O_RDONLY));
	ASSERT_EQ(0, test_open(file2_s1d3, O_WRONLY));

	ruleset_fd = create_ruleset(_metadata,
				    LANDLOCK_ACCESS_FS_READ_FILE |
					    LANDLOCK_ACCESS_FS_WRITE_FILE,
				    layer2_read_write);
	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/* Checks that previous access rights are unchanged with layer 2. */
	ASSERT_EQ(0, test_open(file1_s1d3, O_RDWR));
	ASSERT_EQ(EACCES, test_open(file2_s1d3, O_RDONLY));
	ASSERT_EQ(0, test_open(file2_s1d3, O_WRONLY));

	ruleset_fd = create_ruleset(_metadata, LANDLOCK_ACCESS_FS_READ_FILE,
				    layer3_read);
	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/* Checks that previous access rights are unchanged with layer 3. */
	ASSERT_EQ(0, test_open(file1_s1d3, O_RDWR));
	ASSERT_EQ(EACCES, test_open(file2_s1d3, O_RDONLY));
	ASSERT_EQ(0, test_open(file2_s1d3, O_WRONLY));

	/* This time, denies write access for the file hierarchy. */
	ruleset_fd = create_ruleset(_metadata,
				    LANDLOCK_ACCESS_FS_READ_FILE |
					    LANDLOCK_ACCESS_FS_WRITE_FILE,
				    layer4_read_write);
	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/*
	 * Checks that the only change with layer 4 is that write access is
	 * denied.
	 */
	ASSERT_EQ(0, test_open(file1_s1d3, O_RDONLY));
	ASSERT_EQ(EACCES, test_open(file1_s1d3, O_WRONLY));
	ASSERT_EQ(EACCES, test_open(file2_s1d3, O_RDONLY));
	ASSERT_EQ(EACCES, test_open(file2_s1d3, O_WRONLY));

	ruleset_fd = create_ruleset(_metadata, LANDLOCK_ACCESS_FS_READ_FILE,
				    layer5_read);
	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/* Checks that previous access rights are unchanged with layer 5. */
	ASSERT_EQ(0, test_open(file1_s1d3, O_RDONLY));
	ASSERT_EQ(EACCES, test_open(file1_s1d3, O_WRONLY));
	ASSERT_EQ(EACCES, test_open(file2_s1d3, O_WRONLY));
	ASSERT_EQ(EACCES, test_open(file2_s1d3, O_RDONLY));

	ruleset_fd = create_ruleset(_metadata, LANDLOCK_ACCESS_FS_EXECUTE,
				    layer6_execute);
	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/* Checks that previous access rights are unchanged with layer 6. */
	ASSERT_EQ(0, test_open(file1_s1d3, O_RDONLY));
	ASSERT_EQ(EACCES, test_open(file1_s1d3, O_WRONLY));
	ASSERT_EQ(EACCES, test_open(file2_s1d3, O_WRONLY));
	ASSERT_EQ(EACCES, test_open(file2_s1d3, O_RDONLY));

	ruleset_fd = create_ruleset(_metadata,
				    LANDLOCK_ACCESS_FS_READ_FILE |
					    LANDLOCK_ACCESS_FS_WRITE_FILE,
				    layer7_read_write);
	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/* Checks read access is now denied with layer 7. */
	ASSERT_EQ(EACCES, test_open(file1_s1d3, O_RDONLY));
	ASSERT_EQ(EACCES, test_open(file1_s1d3, O_WRONLY));
	ASSERT_EQ(EACCES, test_open(file2_s1d3, O_WRONLY));
	ASSERT_EQ(EACCES, test_open(file2_s1d3, O_RDONLY));
}

TEST_F_FORK(layout1, inherit_subset)
{
	const struct rule rules[] = {
		{
			.path = dir_s1d2,
			.access = LANDLOCK_ACCESS_FS_READ_FILE |
				  LANDLOCK_ACCESS_FS_READ_DIR,
		},
		{},
	};
	const int ruleset_fd = create_ruleset(_metadata, ACCESS_RW, rules);

	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);

	ASSERT_EQ(EACCES, test_open(file1_s1d1, O_WRONLY));
	ASSERT_EQ(EACCES, test_open(dir_s1d1, O_RDONLY | O_DIRECTORY));

	/* Write access is forbidden. */
	ASSERT_EQ(EACCES, test_open(file1_s1d2, O_WRONLY));
	/* Readdir access is allowed. */
	ASSERT_EQ(0, test_open(dir_s1d2, O_RDONLY | O_DIRECTORY));

	/* Write access is forbidden. */
	ASSERT_EQ(EACCES, test_open(file1_s1d3, O_WRONLY));
	/* Readdir access is allowed. */
	ASSERT_EQ(0, test_open(dir_s1d3, O_RDONLY | O_DIRECTORY));

	/*
	 * Tests shared rule extension: the following rules should not grant
	 * any new access, only remove some.  Once enforced, these rules are
	 * ANDed with the previous ones.
	 */
	add_path_beneath(_metadata, ruleset_fd, LANDLOCK_ACCESS_FS_WRITE_FILE,
			 dir_s1d2);
	/*
	 * According to ruleset_fd, dir_s1d2 should now have the
	 * LANDLOCK_ACCESS_FS_READ_FILE and LANDLOCK_ACCESS_FS_WRITE_FILE
	 * access rights (even if this directory is opened a second time).
	 * However, when enforcing this updated ruleset, the ruleset tied to
	 * the current process (i.e. its domain) will still only have the
	 * dir_s1d2 with LANDLOCK_ACCESS_FS_READ_FILE and
	 * LANDLOCK_ACCESS_FS_READ_DIR accesses, but
	 * LANDLOCK_ACCESS_FS_WRITE_FILE must not be allowed because it would
	 * be a privilege escalation.
	 */
	enforce_ruleset(_metadata, ruleset_fd);

	/* Same tests and results as above. */
	ASSERT_EQ(EACCES, test_open(file1_s1d1, O_WRONLY));
	ASSERT_EQ(EACCES, test_open(dir_s1d1, O_RDONLY | O_DIRECTORY));

	/* It is still forbidden to write in file1_s1d2. */
	ASSERT_EQ(EACCES, test_open(file1_s1d2, O_WRONLY));
	/* Readdir access is still allowed. */
	ASSERT_EQ(0, test_open(dir_s1d2, O_RDONLY | O_DIRECTORY));

	/* It is still forbidden to write in file1_s1d3. */
	ASSERT_EQ(EACCES, test_open(file1_s1d3, O_WRONLY));
	/* Readdir access is still allowed. */
	ASSERT_EQ(0, test_open(dir_s1d3, O_RDONLY | O_DIRECTORY));

	/*
	 * Try to get more privileges by adding new access rights to the parent
	 * directory: dir_s1d1.
	 */
	add_path_beneath(_metadata, ruleset_fd, ACCESS_RW, dir_s1d1);
	enforce_ruleset(_metadata, ruleset_fd);

	/* Same tests and results as above. */
	ASSERT_EQ(EACCES, test_open(file1_s1d1, O_WRONLY));
	ASSERT_EQ(EACCES, test_open(dir_s1d1, O_RDONLY | O_DIRECTORY));

	/* It is still forbidden to write in file1_s1d2. */
	ASSERT_EQ(EACCES, test_open(file1_s1d2, O_WRONLY));
	/* Readdir access is still allowed. */
	ASSERT_EQ(0, test_open(dir_s1d2, O_RDONLY | O_DIRECTORY));

	/* It is still forbidden to write in file1_s1d3. */
	ASSERT_EQ(EACCES, test_open(file1_s1d3, O_WRONLY));
	/* Readdir access is still allowed. */
	ASSERT_EQ(0, test_open(dir_s1d3, O_RDONLY | O_DIRECTORY));

	/*
	 * Now, dir_s1d3 get a new rule tied to it, only allowing
	 * LANDLOCK_ACCESS_FS_WRITE_FILE.  The (kernel internal) difference is
	 * that there was no rule tied to it before.
	 */
	add_path_beneath(_metadata, ruleset_fd, LANDLOCK_ACCESS_FS_WRITE_FILE,
			 dir_s1d3);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/*
	 * Same tests and results as above, except for open(dir_s1d3) which is
	 * now denied because the new rule mask the rule previously inherited
	 * from dir_s1d2.
	 */

	/* Same tests and results as above. */
	ASSERT_EQ(EACCES, test_open(file1_s1d1, O_WRONLY));
	ASSERT_EQ(EACCES, test_open(dir_s1d1, O_RDONLY | O_DIRECTORY));

	/* It is still forbidden to write in file1_s1d2. */
	ASSERT_EQ(EACCES, test_open(file1_s1d2, O_WRONLY));
	/* Readdir access is still allowed. */
	ASSERT_EQ(0, test_open(dir_s1d2, O_RDONLY | O_DIRECTORY));

	/* It is still forbidden to write in file1_s1d3. */
	ASSERT_EQ(EACCES, test_open(file1_s1d3, O_WRONLY));
	/*
	 * Readdir of dir_s1d3 is still allowed because of the OR policy inside
	 * the same layer.
	 */
	ASSERT_EQ(0, test_open(dir_s1d3, O_RDONLY | O_DIRECTORY));
}

TEST_F_FORK(layout1, inherit_superset)
{
	const struct rule rules[] = {
		{
			.path = dir_s1d3,
			.access = ACCESS_RO,
		},
		{},
	};
	const int ruleset_fd = create_ruleset(_metadata, ACCESS_RW, rules);

	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);

	/* Readdir access is denied for dir_s1d2. */
	ASSERT_EQ(EACCES, test_open(dir_s1d2, O_RDONLY | O_DIRECTORY));
	/* Readdir access is allowed for dir_s1d3. */
	ASSERT_EQ(0, test_open(dir_s1d3, O_RDONLY | O_DIRECTORY));
	/* File access is allowed for file1_s1d3. */
	ASSERT_EQ(0, test_open(file1_s1d3, O_RDONLY));

	/* Now dir_s1d2, parent of dir_s1d3, gets a new rule tied to it. */
	add_path_beneath(_metadata, ruleset_fd,
			 LANDLOCK_ACCESS_FS_READ_FILE |
				 LANDLOCK_ACCESS_FS_READ_DIR,
			 dir_s1d2);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/* Readdir access is still denied for dir_s1d2. */
	ASSERT_EQ(EACCES, test_open(dir_s1d2, O_RDONLY | O_DIRECTORY));
	/* Readdir access is still allowed for dir_s1d3. */
	ASSERT_EQ(0, test_open(dir_s1d3, O_RDONLY | O_DIRECTORY));
	/* File access is still allowed for file1_s1d3. */
	ASSERT_EQ(0, test_open(file1_s1d3, O_RDONLY));
}

TEST_F_FORK(layout0, max_layers)
{
	int i, err;
	const struct rule rules[] = {
		{
			.path = TMP_DIR,
			.access = ACCESS_RO,
		},
		{},
	};
	const int ruleset_fd = create_ruleset(_metadata, ACCESS_RW, rules);

	ASSERT_LE(0, ruleset_fd);
	for (i = 0; i < 16; i++)
		enforce_ruleset(_metadata, ruleset_fd);

	for (i = 0; i < 2; i++) {
		err = landlock_restrict_self(ruleset_fd, 0);
		ASSERT_EQ(-1, err);
		ASSERT_EQ(E2BIG, errno);
	}
	ASSERT_EQ(0, close(ruleset_fd));
}

TEST_F_FORK(layout1, empty_or_same_ruleset)
{
	struct landlock_ruleset_attr ruleset_attr = {};
	int ruleset_fd;

	/* Tests empty handled_access_fs. */
	ruleset_fd =
		landlock_create_ruleset(&ruleset_attr, sizeof(ruleset_attr), 0);
	ASSERT_LE(-1, ruleset_fd);
	ASSERT_EQ(ENOMSG, errno);

	/* Enforces policy which deny read access to all files. */
	ruleset_attr.handled_access_fs = LANDLOCK_ACCESS_FS_READ_FILE;
	ruleset_fd =
		landlock_create_ruleset(&ruleset_attr, sizeof(ruleset_attr), 0);
	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(EACCES, test_open(file1_s1d1, O_RDONLY));
	ASSERT_EQ(0, test_open(dir_s1d1, O_RDONLY));

	/* Nests a policy which deny read access to all directories. */
	ruleset_attr.handled_access_fs = LANDLOCK_ACCESS_FS_READ_DIR;
	ruleset_fd =
		landlock_create_ruleset(&ruleset_attr, sizeof(ruleset_attr), 0);
	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(EACCES, test_open(file1_s1d1, O_RDONLY));
	ASSERT_EQ(EACCES, test_open(dir_s1d1, O_RDONLY));

	/* Enforces a second time with the same ruleset. */
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));
}

TEST_F_FORK(layout1, rule_on_mountpoint)
{
	const struct rule rules[] = {
		{
			.path = dir_s1d1,
			.access = ACCESS_RO,
		},
		{
			/* dir_s3d2 is a mount point. */
			.path = dir_s3d2,
			.access = ACCESS_RO,
		},
		{},
	};
	const int ruleset_fd = create_ruleset(_metadata, ACCESS_RW, rules);

	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	ASSERT_EQ(0, test_open(dir_s1d1, O_RDONLY));

	ASSERT_EQ(EACCES, test_open(dir_s2d1, O_RDONLY));

	ASSERT_EQ(EACCES, test_open(dir_s3d1, O_RDONLY));
	ASSERT_EQ(0, test_open(dir_s3d2, O_RDONLY));
	ASSERT_EQ(0, test_open(dir_s3d3, O_RDONLY));
}

TEST_F_FORK(layout1, rule_over_mountpoint)
{
	const struct rule rules[] = {
		{
			.path = dir_s1d1,
			.access = ACCESS_RO,
		},
		{
			/* dir_s3d2 is a mount point. */
			.path = dir_s3d1,
			.access = ACCESS_RO,
		},
		{},
	};
	const int ruleset_fd = create_ruleset(_metadata, ACCESS_RW, rules);

	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	ASSERT_EQ(0, test_open(dir_s1d1, O_RDONLY));

	ASSERT_EQ(EACCES, test_open(dir_s2d1, O_RDONLY));

	ASSERT_EQ(0, test_open(dir_s3d1, O_RDONLY));
	ASSERT_EQ(0, test_open(dir_s3d2, O_RDONLY));
	ASSERT_EQ(0, test_open(dir_s3d3, O_RDONLY));
}

/*
 * This test verifies that we can apply a landlock rule on the root directory
 * (which might require special handling).
 */
TEST_F_FORK(layout1, rule_over_root_allow_then_deny)
{
	struct rule rules[] = {
		{
			.path = "/",
			.access = ACCESS_RO,
		},
		{},
	};
	int ruleset_fd = create_ruleset(_metadata, ACCESS_RW, rules);

	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/* Checks allowed access. */
	ASSERT_EQ(0, test_open("/", O_RDONLY));
	ASSERT_EQ(0, test_open(dir_s1d1, O_RDONLY));

	rules[0].access = LANDLOCK_ACCESS_FS_READ_FILE;
	ruleset_fd = create_ruleset(_metadata, ACCESS_RW, rules);
	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/* Checks denied access (on a directory). */
	ASSERT_EQ(EACCES, test_open("/", O_RDONLY));
	ASSERT_EQ(EACCES, test_open(dir_s1d1, O_RDONLY));
}

TEST_F_FORK(layout1, rule_over_root_deny)
{
	const struct rule rules[] = {
		{
			.path = "/",
			.access = LANDLOCK_ACCESS_FS_READ_FILE,
		},
		{},
	};
	const int ruleset_fd = create_ruleset(_metadata, ACCESS_RW, rules);

	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/* Checks denied access (on a directory). */
	ASSERT_EQ(EACCES, test_open("/", O_RDONLY));
	ASSERT_EQ(EACCES, test_open(dir_s1d1, O_RDONLY));
}

TEST_F_FORK(layout1, rule_inside_mount_ns)
{
	const struct rule rules[] = {
		{
			.path = "s3d3",
			.access = ACCESS_RO,
		},
		{},
	};
	int ruleset_fd;

	set_cap(_metadata, CAP_SYS_ADMIN);
	ASSERT_EQ(0, syscall(__NR_pivot_root, dir_s3d2, dir_s3d3))
	{
		TH_LOG("Failed to pivot root: %s", strerror(errno));
	};
	ASSERT_EQ(0, chdir("/"));
	clear_cap(_metadata, CAP_SYS_ADMIN);

	ruleset_fd = create_ruleset(_metadata, ACCESS_RW, rules);
	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	ASSERT_EQ(0, test_open("s3d3", O_RDONLY));
	ASSERT_EQ(EACCES, test_open("/", O_RDONLY));
}

TEST_F_FORK(layout1, mount_and_pivot)
{
	const struct rule rules[] = {
		{
			.path = dir_s3d2,
			.access = ACCESS_RO,
		},
		{},
	};
	const int ruleset_fd = create_ruleset(_metadata, ACCESS_RW, rules);

	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	set_cap(_metadata, CAP_SYS_ADMIN);
	ASSERT_EQ(-1, mount(NULL, dir_s3d2, NULL, MS_RDONLY, NULL));
	ASSERT_EQ(EPERM, errno);
	ASSERT_EQ(-1, syscall(__NR_pivot_root, dir_s3d2, dir_s3d3));
	ASSERT_EQ(EPERM, errno);
	clear_cap(_metadata, CAP_SYS_ADMIN);
}

TEST_F_FORK(layout1, move_mount)
{
	const struct rule rules[] = {
		{
			.path = dir_s3d2,
			.access = ACCESS_RO,
		},
		{},
	};
	const int ruleset_fd = create_ruleset(_metadata, ACCESS_RW, rules);

	ASSERT_LE(0, ruleset_fd);

	set_cap(_metadata, CAP_SYS_ADMIN);
	ASSERT_EQ(0, syscall(__NR_move_mount, AT_FDCWD, dir_s3d2, AT_FDCWD,
			     dir_s1d2, 0))
	{
		TH_LOG("Failed to move mount: %s", strerror(errno));
	}

	ASSERT_EQ(0, syscall(__NR_move_mount, AT_FDCWD, dir_s1d2, AT_FDCWD,
			     dir_s3d2, 0));
	clear_cap(_metadata, CAP_SYS_ADMIN);

	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	set_cap(_metadata, CAP_SYS_ADMIN);
	ASSERT_EQ(-1, syscall(__NR_move_mount, AT_FDCWD, dir_s3d2, AT_FDCWD,
			      dir_s1d2, 0));
	ASSERT_EQ(EPERM, errno);
	clear_cap(_metadata, CAP_SYS_ADMIN);
}

TEST_F_FORK(layout1, topology_changes_with_net_only)
{
	const struct landlock_ruleset_attr ruleset_net = {
		.handled_access_net = LANDLOCK_ACCESS_NET_BIND_TCP |
				      LANDLOCK_ACCESS_NET_CONNECT_TCP,
	};
	int ruleset_fd;

	/* Add network restrictions. */
	ruleset_fd =
		landlock_create_ruleset(&ruleset_net, sizeof(ruleset_net), 0);
	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/* Mount, remount, move_mount, umount, and pivot_root checks. */
	set_cap(_metadata, CAP_SYS_ADMIN);
	ASSERT_EQ(0, mount_opt(&mnt_tmp, dir_s1d2));
	ASSERT_EQ(0, mount(NULL, dir_s1d2, NULL, MS_PRIVATE | MS_REC, NULL));
	ASSERT_EQ(0, syscall(__NR_move_mount, AT_FDCWD, dir_s1d2, AT_FDCWD,
			     dir_s2d2, 0));
	ASSERT_EQ(0, umount(dir_s2d2));
	ASSERT_EQ(0, syscall(__NR_pivot_root, dir_s3d2, dir_s3d3));
	ASSERT_EQ(0, chdir("/"));
	clear_cap(_metadata, CAP_SYS_ADMIN);
}

TEST_F_FORK(layout1, topology_changes_with_net_and_fs)
{
	const struct landlock_ruleset_attr ruleset_net_fs = {
		.handled_access_net = LANDLOCK_ACCESS_NET_BIND_TCP |
				      LANDLOCK_ACCESS_NET_CONNECT_TCP,
		.handled_access_fs = LANDLOCK_ACCESS_FS_EXECUTE,
	};
	int ruleset_fd;

	/* Add network and filesystem restrictions. */
	ruleset_fd = landlock_create_ruleset(&ruleset_net_fs,
					     sizeof(ruleset_net_fs), 0);
	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/* Mount, remount, move_mount, umount, and pivot_root checks. */
	set_cap(_metadata, CAP_SYS_ADMIN);
	ASSERT_EQ(-1, mount_opt(&mnt_tmp, dir_s1d2));
	ASSERT_EQ(EPERM, errno);
	ASSERT_EQ(-1, mount(NULL, dir_s3d2, NULL, MS_PRIVATE | MS_REC, NULL));
	ASSERT_EQ(EPERM, errno);
	ASSERT_EQ(-1, syscall(__NR_move_mount, AT_FDCWD, dir_s3d2, AT_FDCWD,
			      dir_s2d2, 0));
	ASSERT_EQ(EPERM, errno);
	ASSERT_EQ(-1, umount(dir_s3d2));
	ASSERT_EQ(EPERM, errno);
	ASSERT_EQ(-1, syscall(__NR_pivot_root, dir_s3d2, dir_s3d3));
	ASSERT_EQ(EPERM, errno);
	clear_cap(_metadata, CAP_SYS_ADMIN);
}

TEST_F_FORK(layout1, release_inodes)
{
	const struct rule rules[] = {
		{
			.path = dir_s1d1,
			.access = ACCESS_RO,
		},
		{
			.path = dir_s3d2,
			.access = ACCESS_RO,
		},
		{
			.path = dir_s3d3,
			.access = ACCESS_RO,
		},
		{},
	};
	const int ruleset_fd = create_ruleset(_metadata, ACCESS_RW, rules);

	ASSERT_LE(0, ruleset_fd);
	/* Unmount a file hierarchy while it is being used by a ruleset. */
	set_cap(_metadata, CAP_SYS_ADMIN);
	ASSERT_EQ(0, umount(dir_s3d2));
	clear_cap(_metadata, CAP_SYS_ADMIN);

	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	ASSERT_EQ(0, test_open(file1_s1d1, O_RDONLY));
	ASSERT_EQ(EACCES, test_open(dir_s3d2, O_RDONLY));
	/* This dir_s3d3 would not be allowed and does not exist anyway. */
	ASSERT_EQ(ENOENT, test_open(dir_s3d3, O_RDONLY));
}

/*
 * This test checks that a rule on a directory used as a mount point does not
 * grant access to the mount covering it.  It is a generalization of the bind
 * mount case in layout3_fs.hostfs.release_inodes that tests hidden mount points.
 */
TEST_F_FORK(layout1, covered_rule)
{
	const struct rule layer1[] = {
		{
			.path = dir_s3d2,
			.access = LANDLOCK_ACCESS_FS_READ_DIR,
		},
		{},
	};
	int ruleset_fd;

	/* Unmount to simplify FIXTURE_TEARDOWN. */
	set_cap(_metadata, CAP_SYS_ADMIN);
	ASSERT_EQ(0, umount(dir_s3d2));
	clear_cap(_metadata, CAP_SYS_ADMIN);

	/* Creates a ruleset with the future hidden directory. */
	ruleset_fd =
		create_ruleset(_metadata, LANDLOCK_ACCESS_FS_READ_DIR, layer1);
	ASSERT_LE(0, ruleset_fd);

	/* Covers with a new mount point. */
	set_cap(_metadata, CAP_SYS_ADMIN);
	ASSERT_EQ(0, mount_opt(&mnt_tmp, dir_s3d2));
	clear_cap(_metadata, CAP_SYS_ADMIN);

	ASSERT_EQ(0, test_open(dir_s3d2, O_RDONLY));

	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/* Checks that access to the new mount point is denied. */
	ASSERT_EQ(EACCES, test_open(dir_s3d2, O_RDONLY));
}

enum relative_access {
	REL_OPEN,
	REL_CHDIR,
	REL_CHROOT_ONLY,
	REL_CHROOT_CHDIR,
};

static void test_relative_path(struct __test_metadata *const _metadata,
			       const enum relative_access rel)
{
	/*
	 * Common layer to check that chroot doesn't ignore it (i.e. a chroot
	 * is not a disconnected root directory).
	 */
	const struct rule layer1_base[] = {
		{
			.path = TMP_DIR,
			.access = ACCESS_RO,
		},
		{},
	};
	const struct rule layer2_subs[] = {
		{
			.path = dir_s1d2,
			.access = ACCESS_RO,
		},
		{
			.path = dir_s2d2,
			.access = ACCESS_RO,
		},
		{},
	};
	int dirfd, ruleset_fd;

	ruleset_fd = create_ruleset(_metadata, ACCESS_RW, layer1_base);
	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	ruleset_fd = create_ruleset(_metadata, ACCESS_RW, layer2_subs);

	ASSERT_LE(0, ruleset_fd);
	switch (rel) {
	case REL_OPEN:
	case REL_CHDIR:
		break;
	case REL_CHROOT_ONLY:
		ASSERT_EQ(0, chdir(dir_s2d2));
		break;
	case REL_CHROOT_CHDIR:
		ASSERT_EQ(0, chdir(dir_s1d2));
		break;
	default:
		ASSERT_TRUE(false);
		return;
	}

	set_cap(_metadata, CAP_SYS_CHROOT);
	enforce_ruleset(_metadata, ruleset_fd);

	switch (rel) {
	case REL_OPEN:
		dirfd = open(dir_s1d2, O_DIRECTORY);
		ASSERT_LE(0, dirfd);
		break;
	case REL_CHDIR:
		ASSERT_EQ(0, chdir(dir_s1d2));
		dirfd = AT_FDCWD;
		break;
	case REL_CHROOT_ONLY:
		/* Do chroot into dir_s1d2 (relative to dir_s2d2). */
		ASSERT_EQ(0, chroot("../../s1d1/s1d2"))
		{
			TH_LOG("Failed to chroot: %s", strerror(errno));
		}
		dirfd = AT_FDCWD;
		break;
	case REL_CHROOT_CHDIR:
		/* Do chroot into dir_s1d2. */
		ASSERT_EQ(0, chroot("."))
		{
			TH_LOG("Failed to chroot: %s", strerror(errno));
		}
		dirfd = AT_FDCWD;
		break;
	}

	ASSERT_EQ((rel == REL_CHROOT_CHDIR) ? 0 : EACCES,
		  test_open_rel(dirfd, "..", O_RDONLY));
	ASSERT_EQ(0, test_open_rel(dirfd, ".", O_RDONLY));

	if (rel == REL_CHROOT_ONLY) {
		/* The current directory is dir_s2d2. */
		ASSERT_EQ(0, test_open_rel(dirfd, "./s2d3", O_RDONLY));
	} else {
		/* The current directory is dir_s1d2. */
		ASSERT_EQ(0, test_open_rel(dirfd, "./s1d3", O_RDONLY));
	}

	if (rel == REL_CHROOT_ONLY || rel == REL_CHROOT_CHDIR) {
		/* Checks the root dir_s1d2. */
		ASSERT_EQ(0, test_open_rel(dirfd, "/..", O_RDONLY));
		ASSERT_EQ(0, test_open_rel(dirfd, "/", O_RDONLY));
		ASSERT_EQ(0, test_open_rel(dirfd, "/f1", O_RDONLY));
		ASSERT_EQ(0, test_open_rel(dirfd, "/s1d3", O_RDONLY));
	}

	if (rel != REL_CHROOT_CHDIR) {
		ASSERT_EQ(EACCES, test_open_rel(dirfd, "../../s1d1", O_RDONLY));
		ASSERT_EQ(0, test_open_rel(dirfd, "../../s1d1/s1d2", O_RDONLY));
		ASSERT_EQ(0, test_open_rel(dirfd, "../../s1d1/s1d2/s1d3",
					   O_RDONLY));

		ASSERT_EQ(EACCES, test_open_rel(dirfd, "../../s2d1", O_RDONLY));
		ASSERT_EQ(0, test_open_rel(dirfd, "../../s2d1/s2d2", O_RDONLY));
		ASSERT_EQ(0, test_open_rel(dirfd, "../../s2d1/s2d2/s2d3",
					   O_RDONLY));
	}

	if (rel == REL_OPEN)
		ASSERT_EQ(0, close(dirfd));
	ASSERT_EQ(0, close(ruleset_fd));
}

TEST_F_FORK(layout1, relative_open)
{
	test_relative_path(_metadata, REL_OPEN);
}

TEST_F_FORK(layout1, relative_chdir)
{
	test_relative_path(_metadata, REL_CHDIR);
}

TEST_F_FORK(layout1, relative_chroot_only)
{
	test_relative_path(_metadata, REL_CHROOT_ONLY);
}

TEST_F_FORK(layout1, relative_chroot_chdir)
{
	test_relative_path(_metadata, REL_CHROOT_CHDIR);
}

static void copy_file(struct __test_metadata *const _metadata,
		      const char *const src_path, const char *const dst_path)
{
	int dst_fd, src_fd;
	struct stat statbuf;

	dst_fd = open(dst_path, O_WRONLY | O_TRUNC | O_CLOEXEC);
	ASSERT_LE(0, dst_fd)
	{
		TH_LOG("Failed to open \"%s\": %s", dst_path, strerror(errno));
	}
	src_fd = open(src_path, O_RDONLY | O_CLOEXEC);
	ASSERT_LE(0, src_fd)
	{
		TH_LOG("Failed to open \"%s\": %s", src_path, strerror(errno));
	}
	ASSERT_EQ(0, fstat(src_fd, &statbuf));
	ASSERT_EQ(statbuf.st_size,
		  sendfile(dst_fd, src_fd, 0, statbuf.st_size));
	ASSERT_EQ(0, close(src_fd));
	ASSERT_EQ(0, close(dst_fd));
}

static void test_execute(struct __test_metadata *const _metadata, const int err,
			 const char *const path)
{
	int status;
	char *const argv[] = { (char *)path, NULL };
	const pid_t child = fork();

	ASSERT_LE(0, child);
	if (child == 0) {
		ASSERT_EQ(err ? -1 : 0, execve(path, argv, NULL))
		{
			TH_LOG("Failed to execute \"%s\": %s", path,
			       strerror(errno));
		};
		ASSERT_EQ(err, errno);
		_exit(__test_passed(_metadata) ? 2 : 1);
		return;
	}
	ASSERT_EQ(child, waitpid(child, &status, 0));
	ASSERT_EQ(1, WIFEXITED(status));
	ASSERT_EQ(err ? 2 : 0, WEXITSTATUS(status))
	{
		TH_LOG("Unexpected return code for \"%s\"", path);
	};
}

static void test_check_exec(struct __test_metadata *const _metadata,
			    const int err, const char *const path)
{
	int ret;
	char *const argv[] = { (char *)path, NULL };

	ret = sys_execveat(AT_FDCWD, path, argv, NULL,
			   AT_EMPTY_PATH | AT_EXECVE_CHECK);
	if (err) {
		EXPECT_EQ(-1, ret);
		EXPECT_EQ(errno, err);
	} else {
		EXPECT_EQ(0, ret);
	}
}

TEST_F_FORK(layout1, execute)
{
	const struct rule rules[] = {
		{
			.path = dir_s1d2,
			.access = LANDLOCK_ACCESS_FS_EXECUTE,
		},
		{},
	};
	const int ruleset_fd =
		create_ruleset(_metadata, rules[0].access, rules);

	ASSERT_LE(0, ruleset_fd);
	copy_file(_metadata, bin_true, file1_s1d1);
	copy_file(_metadata, bin_true, file1_s1d2);
	copy_file(_metadata, bin_true, file1_s1d3);

	/* Checks before file1_s1d1 being denied. */
	test_execute(_metadata, 0, file1_s1d1);
	test_check_exec(_metadata, 0, file1_s1d1);

	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	ASSERT_EQ(0, test_open(dir_s1d1, O_RDONLY));
	ASSERT_EQ(0, test_open(file1_s1d1, O_RDONLY));
	test_execute(_metadata, EACCES, file1_s1d1);
	test_check_exec(_metadata, EACCES, file1_s1d1);

	ASSERT_EQ(0, test_open(dir_s1d2, O_RDONLY));
	ASSERT_EQ(0, test_open(file1_s1d2, O_RDONLY));
	test_execute(_metadata, 0, file1_s1d2);
	test_check_exec(_metadata, 0, file1_s1d2);

	ASSERT_EQ(0, test_open(dir_s1d3, O_RDONLY));
	ASSERT_EQ(0, test_open(file1_s1d3, O_RDONLY));
	test_execute(_metadata, 0, file1_s1d3);
	test_check_exec(_metadata, 0, file1_s1d3);
}

TEST_F_FORK(layout1, umount_sandboxer)
{
	int pipe_child[2], pipe_parent[2];
	char buf_parent;
	pid_t child;
	int status;

	copy_file(_metadata, bin_sandbox_and_launch, file1_s3d3);
	ASSERT_EQ(0, pipe2(pipe_child, 0));
	ASSERT_EQ(0, pipe2(pipe_parent, 0));

	child = fork();
	ASSERT_LE(0, child);
	if (child == 0) {
		char pipe_child_str[12], pipe_parent_str[12];
		char *const argv[] = { (char *)file1_s3d3,
				       (char *)bin_wait_pipe, pipe_child_str,
				       pipe_parent_str, NULL };

		/* Passes the pipe FDs to the executed binary and its child. */
		EXPECT_EQ(0, close(pipe_child[0]));
		EXPECT_EQ(0, close(pipe_parent[1]));
		snprintf(pipe_child_str, sizeof(pipe_child_str), "%d",
			 pipe_child[1]);
		snprintf(pipe_parent_str, sizeof(pipe_parent_str), "%d",
			 pipe_parent[0]);

		/*
		 * We need bin_sandbox_and_launch (copied inside the mount as
		 * file1_s3d3) to execute bin_wait_pipe (outside the mount) to
		 * make sure the mount point will not be EBUSY because of
		 * file1_s3d3 being in use.  This avoids a potential race
		 * condition between the following read() and umount() calls.
		 */
		ASSERT_EQ(0, execve(argv[0], argv, NULL))
		{
			TH_LOG("Failed to execute \"%s\": %s", argv[0],
			       strerror(errno));
		};
		_exit(1);
		return;
	}

	EXPECT_EQ(0, close(pipe_child[1]));
	EXPECT_EQ(0, close(pipe_parent[0]));

	/* Waits for the child to sandbox itself. */
	EXPECT_EQ(1, read(pipe_child[0], &buf_parent, 1));

	/* Tests that the sandboxer is tied to its mount point. */
	set_cap(_metadata, CAP_SYS_ADMIN);
	EXPECT_EQ(-1, umount(dir_s3d2));
	EXPECT_EQ(EBUSY, errno);
	clear_cap(_metadata, CAP_SYS_ADMIN);

	/* Signals the child to launch a grandchild. */
	EXPECT_EQ(1, write(pipe_parent[1], ".", 1));

	/* Waits for the grandchild. */
	EXPECT_EQ(1, read(pipe_child[0], &buf_parent, 1));

	/* Tests that the domain's sandboxer is not tied to its mount point. */
	set_cap(_metadata, CAP_SYS_ADMIN);
	EXPECT_EQ(0, umount(dir_s3d2))
	{
		TH_LOG("Failed to umount \"%s\": %s", dir_s3d2,
		       strerror(errno));
	};
	clear_cap(_metadata, CAP_SYS_ADMIN);

	/* Signals the grandchild to terminate. */
	EXPECT_EQ(1, write(pipe_parent[1], ".", 1));
	ASSERT_EQ(child, waitpid(child, &status, 0));
	ASSERT_EQ(1, WIFEXITED(status));
	ASSERT_EQ(0, WEXITSTATUS(status));
}

TEST_F_FORK(layout1, link)
{
	const struct rule layer1[] = {
		{
			.path = dir_s1d2,
			.access = LANDLOCK_ACCESS_FS_MAKE_REG,
		},
		{},
	};
	const struct rule layer2[] = {
		{
			.path = dir_s1d3,
			.access = LANDLOCK_ACCESS_FS_REMOVE_FILE,
		},
		{},
	};
	int ruleset_fd = create_ruleset(_metadata, layer1[0].access, layer1);

	ASSERT_LE(0, ruleset_fd);

	ASSERT_EQ(0, unlink(file1_s1d1));
	ASSERT_EQ(0, unlink(file1_s1d2));
	ASSERT_EQ(0, unlink(file1_s1d3));

	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	ASSERT_EQ(-1, link(file2_s1d1, file1_s1d1));
	ASSERT_EQ(EACCES, errno);

	/* Denies linking because of reparenting. */
	ASSERT_EQ(-1, link(file1_s2d1, file1_s1d2));
	ASSERT_EQ(EXDEV, errno);
	ASSERT_EQ(-1, link(file2_s1d2, file1_s1d3));
	ASSERT_EQ(EXDEV, errno);
	ASSERT_EQ(-1, link(file2_s1d3, file1_s1d2));
	ASSERT_EQ(EXDEV, errno);

	ASSERT_EQ(0, link(file2_s1d2, file1_s1d2));
	ASSERT_EQ(0, link(file2_s1d3, file1_s1d3));

	/* Prepares for next unlinks. */
	ASSERT_EQ(0, unlink(file2_s1d2));
	ASSERT_EQ(0, unlink(file2_s1d3));

	ruleset_fd = create_ruleset(_metadata, layer2[0].access, layer2);
	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/* Checks that linkind doesn't require the ability to delete a file. */
	ASSERT_EQ(0, link(file1_s1d2, file2_s1d2));
	ASSERT_EQ(0, link(file1_s1d3, file2_s1d3));
}

static int test_rename(const char *const oldpath, const char *const newpath)
{
	if (rename(oldpath, newpath))
		return errno;
	return 0;
}

static int test_exchange(const char *const oldpath, const char *const newpath)
{
	if (renameat2(AT_FDCWD, oldpath, AT_FDCWD, newpath, RENAME_EXCHANGE))
		return errno;
	return 0;
}

TEST_F_FORK(layout1, rename_file)
{
	const struct rule rules[] = {
		{
			.path = dir_s1d3,
			.access = LANDLOCK_ACCESS_FS_REMOVE_FILE,
		},
		{
			.path = dir_s2d2,
			.access = LANDLOCK_ACCESS_FS_REMOVE_FILE,
		},
		{},
	};
	const int ruleset_fd =
		create_ruleset(_metadata, rules[0].access, rules);

	ASSERT_LE(0, ruleset_fd);

	ASSERT_EQ(0, unlink(file1_s1d2));

	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/*
	 * Tries to replace a file, from a directory that allows file removal,
	 * but to a different directory (which also allows file removal).
	 */
	ASSERT_EQ(-1, rename(file1_s2d3, file1_s1d3));
	ASSERT_EQ(EXDEV, errno);
	ASSERT_EQ(-1, renameat2(AT_FDCWD, file1_s2d3, AT_FDCWD, file1_s1d3,
				RENAME_EXCHANGE));
	ASSERT_EQ(EXDEV, errno);
	ASSERT_EQ(-1, renameat2(AT_FDCWD, file1_s2d3, AT_FDCWD, dir_s1d3,
				RENAME_EXCHANGE));
	ASSERT_EQ(EXDEV, errno);

	/*
	 * Tries to replace a file, from a directory that denies file removal,
	 * to a different directory (which allows file removal).
	 */
	ASSERT_EQ(-1, rename(file1_s2d1, file1_s1d3));
	ASSERT_EQ(EACCES, errno);
	ASSERT_EQ(-1, renameat2(AT_FDCWD, file1_s2d1, AT_FDCWD, file1_s1d3,
				RENAME_EXCHANGE));
	ASSERT_EQ(EACCES, errno);
	ASSERT_EQ(-1, renameat2(AT_FDCWD, dir_s2d2, AT_FDCWD, file1_s1d3,
				RENAME_EXCHANGE));
	ASSERT_EQ(EXDEV, errno);

	/* Exchanges files and directories that partially allow removal. */
	ASSERT_EQ(-1, renameat2(AT_FDCWD, dir_s2d2, AT_FDCWD, file1_s2d1,
				RENAME_EXCHANGE));
	ASSERT_EQ(EACCES, errno);
	/* Checks that file1_s2d1 cannot be removed (instead of ENOTDIR). */
	ASSERT_EQ(-1, rename(dir_s2d2, file1_s2d1));
	ASSERT_EQ(EACCES, errno);
	ASSERT_EQ(-1, renameat2(AT_FDCWD, file1_s2d1, AT_FDCWD, dir_s2d2,
				RENAME_EXCHANGE));
	ASSERT_EQ(EACCES, errno);
	/* Checks that file1_s1d1 cannot be removed (instead of EISDIR). */
	ASSERT_EQ(-1, rename(file1_s1d1, dir_s1d2));
	ASSERT_EQ(EACCES, errno);

	/* Renames files with different parents. */
	ASSERT_EQ(-1, rename(file1_s2d2, file1_s1d2));
	ASSERT_EQ(EXDEV, errno);
	ASSERT_EQ(0, unlink(file1_s1d3));
	ASSERT_EQ(-1, rename(file1_s2d1, file1_s1d3));
	ASSERT_EQ(EACCES, errno);

	/* Exchanges and renames files with same parent. */
	ASSERT_EQ(0, renameat2(AT_FDCWD, file2_s2d3, AT_FDCWD, file1_s2d3,
			       RENAME_EXCHANGE));
	ASSERT_EQ(0, rename(file2_s2d3, file1_s2d3));

	/* Exchanges files and directories with same parent, twice. */
	ASSERT_EQ(0, renameat2(AT_FDCWD, file1_s2d2, AT_FDCWD, dir_s2d3,
			       RENAME_EXCHANGE));
	ASSERT_EQ(0, renameat2(AT_FDCWD, file1_s2d2, AT_FDCWD, dir_s2d3,
			       RENAME_EXCHANGE));
}

TEST_F_FORK(layout1, rename_dir)
{
	const struct rule rules[] = {
		{
			.path = dir_s1d2,
			.access = LANDLOCK_ACCESS_FS_REMOVE_DIR,
		},
		{
			.path = dir_s2d1,
			.access = LANDLOCK_ACCESS_FS_REMOVE_DIR,
		},
		{},
	};
	const int ruleset_fd =
		create_ruleset(_metadata, rules[0].access, rules);

	ASSERT_LE(0, ruleset_fd);

	/* Empties dir_s1d3 to allow renaming. */
	ASSERT_EQ(0, unlink(file1_s1d3));
	ASSERT_EQ(0, unlink(file2_s1d3));

	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/* Exchanges and renames directory to a different parent. */
	ASSERT_EQ(-1, renameat2(AT_FDCWD, dir_s2d3, AT_FDCWD, dir_s1d3,
				RENAME_EXCHANGE));
	ASSERT_EQ(EXDEV, errno);
	ASSERT_EQ(-1, rename(dir_s2d3, dir_s1d3));
	ASSERT_EQ(EXDEV, errno);
	ASSERT_EQ(-1, renameat2(AT_FDCWD, file1_s2d2, AT_FDCWD, dir_s1d3,
				RENAME_EXCHANGE));
	ASSERT_EQ(EXDEV, errno);

	/*
	 * Exchanges directory to the same parent, which doesn't allow
	 * directory removal.
	 */
	ASSERT_EQ(-1, renameat2(AT_FDCWD, dir_s1d1, AT_FDCWD, dir_s2d1,
				RENAME_EXCHANGE));
	ASSERT_EQ(EACCES, errno);
	/* Checks that dir_s1d2 cannot be removed (instead of ENOTDIR). */
	ASSERT_EQ(-1, rename(dir_s1d2, file1_s1d1));
	ASSERT_EQ(EACCES, errno);
	ASSERT_EQ(-1, renameat2(AT_FDCWD, file1_s1d1, AT_FDCWD, dir_s1d2,
				RENAME_EXCHANGE));
	ASSERT_EQ(EACCES, errno);
	/* Checks that dir_s1d2 cannot be removed (instead of EISDIR). */
	ASSERT_EQ(-1, rename(file1_s1d1, dir_s1d2));
	ASSERT_EQ(EACCES, errno);

	/*
	 * Exchanges and renames directory to the same parent, which allows
	 * directory removal.
	 */
	ASSERT_EQ(0, renameat2(AT_FDCWD, dir_s1d3, AT_FDCWD, file1_s1d2,
			       RENAME_EXCHANGE));
	ASSERT_EQ(0, unlink(dir_s1d3));
	ASSERT_EQ(0, mkdir(dir_s1d3, 0700));
	ASSERT_EQ(0, rename(file1_s1d2, dir_s1d3));
	ASSERT_EQ(0, rmdir(dir_s1d3));
}

TEST_F_FORK(layout1, reparent_refer)
{
	const struct rule layer1[] = {
		{
			.path = dir_s1d2,
			.access = LANDLOCK_ACCESS_FS_REFER,
		},
		{
			.path = dir_s2d2,
			.access = LANDLOCK_ACCESS_FS_REFER,
		},
		{},
	};
	int ruleset_fd =
		create_ruleset(_metadata, LANDLOCK_ACCESS_FS_REFER, layer1);

	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	ASSERT_EQ(-1, rename(dir_s1d2, dir_s2d1));
	ASSERT_EQ(EXDEV, errno);
	ASSERT_EQ(-1, rename(dir_s1d2, dir_s2d2));
	ASSERT_EQ(EXDEV, errno);
	ASSERT_EQ(-1, rename(dir_s1d2, dir_s2d3));
	ASSERT_EQ(EXDEV, errno);

	ASSERT_EQ(-1, rename(dir_s1d3, dir_s2d1));
	ASSERT_EQ(EXDEV, errno);
	ASSERT_EQ(-1, rename(dir_s1d3, dir_s2d2));
	ASSERT_EQ(EXDEV, errno);
	/*
	 * Moving should only be allowed when the source and the destination
	 * parent directory have REFER.
	 */
	ASSERT_EQ(-1, rename(dir_s1d3, dir_s2d3));
	ASSERT_EQ(ENOTEMPTY, errno);
	ASSERT_EQ(0, unlink(file1_s2d3));
	ASSERT_EQ(0, unlink(file2_s2d3));
	ASSERT_EQ(0, rename(dir_s1d3, dir_s2d3));
}

/* Checks renames beneath dir_s1d1. */
static void refer_denied_by_default(struct __test_metadata *const _metadata,
				    const struct rule layer1[],
				    const int layer1_err,
				    const struct rule layer2[])
{
	int ruleset_fd;

	ASSERT_EQ(0, unlink(file1_s1d2));

	ruleset_fd = create_ruleset(_metadata, layer1[0].access, layer1);
	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/*
	 * If the first layer handles LANDLOCK_ACCESS_FS_REFER (according to
	 * layer1_err), then it allows some different-parent renames and links.
	 */
	ASSERT_EQ(layer1_err, test_rename(file1_s1d1, file1_s1d2));
	if (layer1_err == 0)
		ASSERT_EQ(layer1_err, test_rename(file1_s1d2, file1_s1d1));
	ASSERT_EQ(layer1_err, test_exchange(file2_s1d1, file2_s1d2));
	ASSERT_EQ(layer1_err, test_exchange(file2_s1d2, file2_s1d1));

	ruleset_fd = create_ruleset(_metadata, layer2[0].access, layer2);
	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/*
	 * Now, either the first or the second layer does not handle
	 * LANDLOCK_ACCESS_FS_REFER, which means that any different-parent
	 * renames and links are denied, thus making the layer handling
	 * LANDLOCK_ACCESS_FS_REFER null and void.
	 */
	ASSERT_EQ(EXDEV, test_rename(file1_s1d1, file1_s1d2));
	ASSERT_EQ(EXDEV, test_exchange(file2_s1d1, file2_s1d2));
	ASSERT_EQ(EXDEV, test_exchange(file2_s1d2, file2_s1d1));
}

const struct rule layer_dir_s1d1_refer[] = {
	{
		.path = dir_s1d1,
		.access = LANDLOCK_ACCESS_FS_REFER,
	},
	{},
};

const struct rule layer_dir_s1d1_execute[] = {
	{
		/* Matches a parent directory. */
		.path = dir_s1d1,
		.access = LANDLOCK_ACCESS_FS_EXECUTE,
	},
	{},
};

const struct rule layer_dir_s2d1_execute[] = {
	{
		/* Does not match a parent directory. */
		.path = dir_s2d1,
		.access = LANDLOCK_ACCESS_FS_EXECUTE,
	},
	{},
};

/*
 * Tests precedence over renames: denied by default for different parent
 * directories, *with* a rule matching a parent directory, but not directly
 * denying access (with MAKE_REG nor REMOVE).
 */
TEST_F_FORK(layout1, refer_denied_by_default1)
{
	refer_denied_by_default(_metadata, layer_dir_s1d1_refer, 0,
				layer_dir_s1d1_execute);
}

/*
 * Same test but this time turning around the ABI version order: the first
 * layer does not handle LANDLOCK_ACCESS_FS_REFER.
 */
TEST_F_FORK(layout1, refer_denied_by_default2)
{
	refer_denied_by_default(_metadata, layer_dir_s1d1_execute, EXDEV,
				layer_dir_s1d1_refer);
}

/*
 * Tests precedence over renames: denied by default for different parent
 * directories, *without* a rule matching a parent directory, but not directly
 * denying access (with MAKE_REG nor REMOVE).
 */
TEST_F_FORK(layout1, refer_denied_by_default3)
{
	refer_denied_by_default(_metadata, layer_dir_s1d1_refer, 0,
				layer_dir_s2d1_execute);
}

/*
 * Same test but this time turning around the ABI version order: the first
 * layer does not handle LANDLOCK_ACCESS_FS_REFER.
 */
TEST_F_FORK(layout1, refer_denied_by_default4)
{
	refer_denied_by_default(_metadata, layer_dir_s2d1_execute, EXDEV,
				layer_dir_s1d1_refer);
}

/*
 * Tests walking through a denied root mount.
 */
TEST_F_FORK(layout1, refer_mount_root_deny)
{
	const struct landlock_ruleset_attr ruleset_attr = {
		.handled_access_fs = LANDLOCK_ACCESS_FS_MAKE_DIR,
	};
	int root_fd, ruleset_fd;

	/* Creates a mount object from a non-mount point. */
	set_cap(_metadata, CAP_SYS_ADMIN);
	root_fd =
		open_tree(AT_FDCWD, dir_s1d1,
			  AT_EMPTY_PATH | OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC);
	clear_cap(_metadata, CAP_SYS_ADMIN);
	ASSERT_LE(0, root_fd);

	ruleset_fd =
		landlock_create_ruleset(&ruleset_attr, sizeof(ruleset_attr), 0);
	ASSERT_LE(0, ruleset_fd);

	ASSERT_EQ(0, prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0));
	ASSERT_EQ(0, landlock_restrict_self(ruleset_fd, 0));
	EXPECT_EQ(0, close(ruleset_fd));

	/* Link denied by Landlock: EACCES. */
	EXPECT_EQ(-1, linkat(root_fd, ".", root_fd, "does_not_exist", 0));
	EXPECT_EQ(EACCES, errno);

	/* renameat2() always returns EBUSY. */
	EXPECT_EQ(-1, renameat2(root_fd, ".", root_fd, "does_not_exist", 0));
	EXPECT_EQ(EBUSY, errno);

	EXPECT_EQ(0, close(root_fd));
}

TEST_F_FORK(layout1, refer_part_mount_tree_is_allowed)
{
	const struct rule layer1[] = {
		{
			/* Parent mount point. */
			.path = dir_s3d1,
			.access = LANDLOCK_ACCESS_FS_REFER |
				  LANDLOCK_ACCESS_FS_MAKE_REG,
		},
		{
			/*
			 * Removing the source file is allowed because its
			 * access rights are already a superset of the
			 * destination.
			 */
			.path = dir_s3d4,
			.access = LANDLOCK_ACCESS_FS_REFER |
				  LANDLOCK_ACCESS_FS_MAKE_REG |
				  LANDLOCK_ACCESS_FS_REMOVE_FILE,
		},
		{},
	};
	int ruleset_fd;

	ASSERT_EQ(0, unlink(file1_s3d3));
	ruleset_fd = create_ruleset(_metadata,
				    LANDLOCK_ACCESS_FS_REFER |
					    LANDLOCK_ACCESS_FS_MAKE_REG |
					    LANDLOCK_ACCESS_FS_REMOVE_FILE,
				    layer1);

	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	ASSERT_EQ(0, rename(file1_s3d4, file1_s3d3));
}

TEST_F_FORK(layout1, reparent_link)
{
	const struct rule layer1[] = {
		{
			.path = dir_s1d2,
			.access = LANDLOCK_ACCESS_FS_MAKE_REG,
		},
		{
			.path = dir_s1d3,
			.access = LANDLOCK_ACCESS_FS_REFER,
		},
		{
			.path = dir_s2d2,
			.access = LANDLOCK_ACCESS_FS_REFER,
		},
		{
			.path = dir_s2d3,
			.access = LANDLOCK_ACCESS_FS_MAKE_REG,
		},
		{},
	};
	const int ruleset_fd = create_ruleset(
		_metadata,
		LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_REFER, layer1);

	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	ASSERT_EQ(0, unlink(file1_s1d1));
	ASSERT_EQ(0, unlink(file1_s1d2));
	ASSERT_EQ(0, unlink(file1_s1d3));

	/* Denies linking because of missing MAKE_REG. */
	ASSERT_EQ(-1, link(file2_s1d1, file1_s1d1));
	ASSERT_EQ(EACCES, errno);
	/* Denies linking because of missing source and destination REFER. */
	ASSERT_EQ(-1, link(file1_s2d1, file1_s1d2));
	ASSERT_EQ(EXDEV, errno);
	/* Denies linking because of missing source REFER. */
	ASSERT_EQ(-1, link(file1_s2d1, file1_s1d3));
	ASSERT_EQ(EXDEV, errno);

	/* Denies linking because of missing MAKE_REG. */
	ASSERT_EQ(-1, link(file1_s2d2, file1_s1d1));
	ASSERT_EQ(EACCES, errno);
	/* Denies linking because of missing destination REFER. */
	ASSERT_EQ(-1, link(file1_s2d2, file1_s1d2));
	ASSERT_EQ(EXDEV, errno);

	/* Allows linking because of REFER and MAKE_REG. */
	ASSERT_EQ(0, link(file1_s2d2, file1_s1d3));
	ASSERT_EQ(0, unlink(file1_s2d2));
	/* Reverse linking denied because of missing MAKE_REG. */
	ASSERT_EQ(-1, link(file1_s1d3, file1_s2d2));
	ASSERT_EQ(EACCES, errno);
	ASSERT_EQ(0, unlink(file1_s2d3));
	/* Checks reverse linking. */
	ASSERT_EQ(0, link(file1_s1d3, file1_s2d3));
	ASSERT_EQ(0, unlink(file1_s1d3));

	/*
	 * This is OK for a file link, but it should not be allowed for a
	 * directory rename (because of the superset of access rights.
	 */
	ASSERT_EQ(0, link(file1_s2d3, file1_s1d3));
	ASSERT_EQ(0, unlink(file1_s1d3));

	ASSERT_EQ(-1, link(file2_s1d2, file1_s1d3));
	ASSERT_EQ(EXDEV, errno);
	ASSERT_EQ(-1, link(file2_s1d3, file1_s1d2));
	ASSERT_EQ(EXDEV, errno);

	ASSERT_EQ(0, link(file2_s1d2, file1_s1d2));
	ASSERT_EQ(0, link(file2_s1d3, file1_s1d3));
}

TEST_F_FORK(layout1, reparent_rename)
{
	/* Same rules as for reparent_link. */
	const struct rule layer1[] = {
		{
			.path = dir_s1d2,
			.access = LANDLOCK_ACCESS_FS_MAKE_REG,
		},
		{
			.path = dir_s1d3,
			.access = LANDLOCK_ACCESS_FS_REFER,
		},
		{
			.path = dir_s2d2,
			.access = LANDLOCK_ACCESS_FS_REFER,
		},
		{
			.path = dir_s2d3,
			.access = LANDLOCK_ACCESS_FS_MAKE_REG,
		},
		{},
	};
	const int ruleset_fd = create_ruleset(
		_metadata,
		LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_REFER, layer1);

	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	ASSERT_EQ(0, unlink(file1_s1d2));
	ASSERT_EQ(0, unlink(file1_s1d3));

	/* Denies renaming because of missing MAKE_REG. */
	ASSERT_EQ(-1, renameat2(AT_FDCWD, file2_s1d1, AT_FDCWD, file1_s1d1,
				RENAME_EXCHANGE));
	ASSERT_EQ(EACCES, errno);
	ASSERT_EQ(-1, renameat2(AT_FDCWD, file1_s1d1, AT_FDCWD, file2_s1d1,
				RENAME_EXCHANGE));
	ASSERT_EQ(EACCES, errno);
	ASSERT_EQ(0, unlink(file1_s1d1));
	ASSERT_EQ(-1, rename(file2_s1d1, file1_s1d1));
	ASSERT_EQ(EACCES, errno);
	/* Even denies same file exchange. */
	ASSERT_EQ(-1, renameat2(AT_FDCWD, file2_s1d1, AT_FDCWD, file2_s1d1,
				RENAME_EXCHANGE));
	ASSERT_EQ(EACCES, errno);

	/* Denies renaming because of missing source and destination REFER. */
	ASSERT_EQ(-1, rename(file1_s2d1, file1_s1d2));
	ASSERT_EQ(EXDEV, errno);
	/*
	 * Denies renaming because of missing MAKE_REG, source and destination
	 * REFER.
	 */
	ASSERT_EQ(-1, renameat2(AT_FDCWD, file1_s2d1, AT_FDCWD, file2_s1d1,
				RENAME_EXCHANGE));
	ASSERT_EQ(EACCES, errno);
	ASSERT_EQ(-1, renameat2(AT_FDCWD, file2_s1d1, AT_FDCWD, file1_s2d1,
				RENAME_EXCHANGE));
	ASSERT_EQ(EACCES, errno);

	/* Denies renaming because of missing source REFER. */
	ASSERT_EQ(-1, rename(file1_s2d1, file1_s1d3));
	ASSERT_EQ(EXDEV, errno);
	/* Denies renaming because of missing MAKE_REG. */
	ASSERT_EQ(-1, renameat2(AT_FDCWD, file1_s2d1, AT_FDCWD, file2_s1d3,
				RENAME_EXCHANGE));
	ASSERT_EQ(EACCES, errno);

	/* Denies renaming because of missing MAKE_REG. */
	ASSERT_EQ(-1, rename(file1_s2d2, file1_s1d1));
	ASSERT_EQ(EACCES, errno);
	/* Denies renaming because of missing destination REFER*/
	ASSERT_EQ(-1, rename(file1_s2d2, file1_s1d2));
	ASSERT_EQ(EXDEV, errno);

	/* Denies exchange because of one missing MAKE_REG. */
	ASSERT_EQ(-1, renameat2(AT_FDCWD, file1_s2d2, AT_FDCWD, file2_s1d3,
				RENAME_EXCHANGE));
	ASSERT_EQ(EACCES, errno);
	/* Allows renaming because of REFER and MAKE_REG. */
	ASSERT_EQ(0, rename(file1_s2d2, file1_s1d3));

	/* Reverse renaming denied because of missing MAKE_REG. */
	ASSERT_EQ(-1, rename(file1_s1d3, file1_s2d2));
	ASSERT_EQ(EACCES, errno);
	ASSERT_EQ(0, unlink(file1_s2d3));
	ASSERT_EQ(0, rename(file1_s1d3, file1_s2d3));

	/* Tests reverse renaming. */
	ASSERT_EQ(0, rename(file1_s2d3, file1_s1d3));
	ASSERT_EQ(0, renameat2(AT_FDCWD, file2_s2d3, AT_FDCWD, file1_s1d3,
			       RENAME_EXCHANGE));
	ASSERT_EQ(0, rename(file1_s1d3, file1_s2d3));

	/*
	 * This is OK for a file rename, but it should not be allowed for a
	 * directory rename (because of the superset of access rights).
	 */
	ASSERT_EQ(0, rename(file1_s2d3, file1_s1d3));
	ASSERT_EQ(0, rename(file1_s1d3, file1_s2d3));

	/*
	 * Tests superset restrictions applied to directories.  Not only the
	 * dir_s2d3's parent (dir_s2d2) should be taken into account but also
	 * access rights tied to dir_s2d3. dir_s2d2 is missing one access right
	 * compared to dir_s1d3/file1_s1d3 (MAKE_REG) but it is provided
	 * directly by the moved dir_s2d3.
	 */
	ASSERT_EQ(0, rename(dir_s2d3, file1_s1d3));
	ASSERT_EQ(0, rename(file1_s1d3, dir_s2d3));
	/*
	 * The first rename is allowed but not the exchange because dir_s1d3's
	 * parent (dir_s1d2) doesn't have REFER.
	 */
	ASSERT_EQ(-1, renameat2(AT_FDCWD, file1_s2d3, AT_FDCWD, dir_s1d3,
				RENAME_EXCHANGE));
	ASSERT_EQ(EXDEV, errno);
	ASSERT_EQ(-1, renameat2(AT_FDCWD, dir_s1d3, AT_FDCWD, file1_s2d3,
				RENAME_EXCHANGE));
	ASSERT_EQ(EXDEV, errno);
	ASSERT_EQ(-1, rename(file1_s2d3, dir_s1d3));
	ASSERT_EQ(EXDEV, errno);

	ASSERT_EQ(-1, rename(file2_s1d2, file1_s1d3));
	ASSERT_EQ(EXDEV, errno);
	ASSERT_EQ(-1, rename(file2_s1d3, file1_s1d2));
	ASSERT_EQ(EXDEV, errno);

	/* Renaming in the same directory is always allowed. */
	ASSERT_EQ(0, rename(file2_s1d2, file1_s1d2));
	ASSERT_EQ(0, rename(file2_s1d3, file1_s1d3));

	ASSERT_EQ(0, unlink(file1_s1d2));
	/* Denies because of missing source MAKE_REG and destination REFER. */
	ASSERT_EQ(-1, rename(dir_s2d3, file1_s1d2));
	ASSERT_EQ(EXDEV, errno);

	ASSERT_EQ(0, unlink(file1_s1d3));
	/* Denies because of missing source MAKE_REG and REFER. */
	ASSERT_EQ(-1, rename(dir_s2d2, file1_s1d3));
	ASSERT_EQ(EXDEV, errno);
}

static void
reparent_exdev_layers_enforce1(struct __test_metadata *const _metadata)
{
	const struct rule layer1[] = {
		{
			.path = dir_s1d2,
			.access = LANDLOCK_ACCESS_FS_REFER,
		},
		{
			/* Interesting for the layer2 tests. */
			.path = dir_s1d3,
			.access = LANDLOCK_ACCESS_FS_MAKE_REG,
		},
		{
			.path = dir_s2d2,
			.access = LANDLOCK_ACCESS_FS_REFER,
		},
		{
			.path = dir_s2d3,
			.access = LANDLOCK_ACCESS_FS_MAKE_REG,
		},
		{},
	};
	const int ruleset_fd = create_ruleset(
		_metadata,
		LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_REFER, layer1);

	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));
}

static void
reparent_exdev_layers_enforce2(struct __test_metadata *const _metadata)
{
	const struct rule layer2[] = {
		{
			.path = dir_s2d3,
			.access = LANDLOCK_ACCESS_FS_MAKE_DIR,
		},
		{},
	};
	/*
	 * Same checks as before but with a second layer and a new MAKE_DIR
	 * rule (and no explicit handling of REFER).
	 */
	const int ruleset_fd =
		create_ruleset(_metadata, LANDLOCK_ACCESS_FS_MAKE_DIR, layer2);

	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));
}

TEST_F_FORK(layout1, reparent_exdev_layers_rename1)
{
	ASSERT_EQ(0, unlink(file1_s2d2));
	ASSERT_EQ(0, unlink(file1_s2d3));

	reparent_exdev_layers_enforce1(_metadata);

	/*
	 * Moving the dir_s1d3 directory below dir_s2d2 is allowed by Landlock
	 * because it doesn't inherit new access rights.
	 */
	ASSERT_EQ(0, rename(dir_s1d3, file1_s2d2));
	ASSERT_EQ(0, rename(file1_s2d2, dir_s1d3));

	/*
	 * Moving the dir_s1d3 directory below dir_s2d3 is allowed, even if it
	 * gets a new inherited access rights (MAKE_REG), because MAKE_REG is
	 * already allowed for dir_s1d3.
	 */
	ASSERT_EQ(0, rename(dir_s1d3, file1_s2d3));
	ASSERT_EQ(0, rename(file1_s2d3, dir_s1d3));

	/*
	 * However, moving the file1_s1d3 file below dir_s2d3 is allowed
	 * because it cannot inherit MAKE_REG right (which is dedicated to
	 * directories).
	 */
	ASSERT_EQ(0, rename(file1_s1d3, file1_s2d3));

	reparent_exdev_layers_enforce2(_metadata);

	/*
	 * Moving the dir_s1d3 directory below dir_s2d2 is now denied because
	 * MAKE_DIR is not tied to dir_s2d2.
	 */
	ASSERT_EQ(-1, rename(dir_s1d3, file1_s2d2));
	ASSERT_EQ(EACCES, errno);

	/*
	 * Moving the dir_s1d3 directory below dir_s2d3 is forbidden because it
	 * would grants MAKE_REG and MAKE_DIR rights to it.
	 */
	ASSERT_EQ(-1, rename(dir_s1d3, file1_s2d3));
	ASSERT_EQ(EXDEV, errno);

	/*
	 * Moving the file2_s1d3 file below dir_s2d3 is denied because the
	 * second layer does not handle REFER, which is always denied by
	 * default.
	 */
	ASSERT_EQ(-1, rename(file2_s1d3, file1_s2d3));
	ASSERT_EQ(EXDEV, errno);
}

TEST_F_FORK(layout1, reparent_exdev_layers_rename2)
{
	reparent_exdev_layers_enforce1(_metadata);

	/* Checks EACCES predominance over EXDEV. */
	ASSERT_EQ(-1, rename(file1_s1d1, file1_s2d2));
	ASSERT_EQ(EACCES, errno);
	ASSERT_EQ(-1, rename(file1_s1d2, file1_s2d2));
	ASSERT_EQ(EACCES, errno);
	ASSERT_EQ(-1, rename(file1_s1d1, file1_s2d3));
	ASSERT_EQ(EXDEV, errno);
	/* Modify layout! */
	ASSERT_EQ(0, rename(file1_s1d2, file1_s2d3));

	/* Without REFER source. */
	ASSERT_EQ(-1, rename(dir_s1d1, file1_s2d2));
	ASSERT_EQ(EXDEV, errno);
	ASSERT_EQ(-1, rename(dir_s1d2, file1_s2d2));
	ASSERT_EQ(EXDEV, errno);

	reparent_exdev_layers_enforce2(_metadata);

	/* Checks EACCES predominance over EXDEV. */
	ASSERT_EQ(-1, rename(file1_s1d1, file1_s2d2));
	ASSERT_EQ(EACCES, errno);
	/* Checks with actual file2_s1d2. */
	ASSERT_EQ(-1, rename(file2_s1d2, file1_s2d2));
	ASSERT_EQ(EACCES, errno);
	ASSERT_EQ(-1, rename(file1_s1d1, file1_s2d3));
	ASSERT_EQ(EXDEV, errno);
	/*
	 * Modifying the layout is now denied because the second layer does not
	 * handle REFER, which is always denied by default.
	 */
	ASSERT_EQ(-1, rename(file2_s1d2, file1_s2d3));
	ASSERT_EQ(EXDEV, errno);

	/* Without REFER source, EACCES wins over EXDEV. */
	ASSERT_EQ(-1, rename(dir_s1d1, file1_s2d2));
	ASSERT_EQ(EACCES, errno);
	ASSERT_EQ(-1, rename(dir_s1d2, file1_s2d2));
	ASSERT_EQ(EACCES, errno);
}

TEST_F_FORK(layout1, reparent_exdev_layers_exchange1)
{
	const char *const dir_file1_s1d2 = file1_s1d2, *const dir_file2_s2d3 =
							       file2_s2d3;

	ASSERT_EQ(0, unlink(file1_s1d2));
	ASSERT_EQ(0, mkdir(file1_s1d2, 0700));
	ASSERT_EQ(0, unlink(file2_s2d3));
	ASSERT_EQ(0, mkdir(file2_s2d3, 0700));

	reparent_exdev_layers_enforce1(_metadata);

	/* Error predominance with file exchange: returns EXDEV and EACCES. */
	ASSERT_EQ(-1, renameat2(AT_FDCWD, file1_s1d1, AT_FDCWD, file1_s2d3,
				RENAME_EXCHANGE));
	ASSERT_EQ(EACCES, errno);
	ASSERT_EQ(-1, renameat2(AT_FDCWD, file1_s2d3, AT_FDCWD, file1_s1d1,
				RENAME_EXCHANGE));
	ASSERT_EQ(EACCES, errno);

	/*
	 * Checks with directories which creation could be allowed, but denied
	 * because of access rights that would be inherited.
	 */
	ASSERT_EQ(-1, renameat2(AT_FDCWD, dir_file1_s1d2, AT_FDCWD,
				dir_file2_s2d3, RENAME_EXCHANGE));
	ASSERT_EQ(EXDEV, errno);
	ASSERT_EQ(-1, renameat2(AT_FDCWD, dir_file2_s2d3, AT_FDCWD,
				dir_file1_s1d2, RENAME_EXCHANGE));
	ASSERT_EQ(EXDEV, errno);

	/* Checks with same access rights. */
	ASSERT_EQ(0, renameat2(AT_FDCWD, dir_s1d3, AT_FDCWD, dir_s2d3,
			       RENAME_EXCHANGE));
	ASSERT_EQ(0, renameat2(AT_FDCWD, dir_s2d3, AT_FDCWD, dir_s1d3,
			       RENAME_EXCHANGE));

	/* Checks with different (child-only) access rights. */
	ASSERT_EQ(0, renameat2(AT_FDCWD, dir_s2d3, AT_FDCWD, dir_file1_s1d2,
			       RENAME_EXCHANGE));
	ASSERT_EQ(0, renameat2(AT_FDCWD, dir_file1_s1d2, AT_FDCWD, dir_s2d3,
			       RENAME_EXCHANGE));

	/*
	 * Checks that exchange between file and directory are consistent.
	 *
	 * Moving a file (file1_s2d2) to a directory which only grants more
	 * directory-related access rights is allowed, and at the same time
	 * moving a directory (dir_file2_s2d3) to another directory which
	 * grants less access rights is allowed too.
	 *
	 * See layout1.reparent_exdev_layers_exchange3 for inverted arguments.
	 */
	ASSERT_EQ(0, renameat2(AT_FDCWD, file1_s2d2, AT_FDCWD, dir_file2_s2d3,
			       RENAME_EXCHANGE));
	/*
	 * However, moving back the directory is denied because it would get
	 * more access rights than the current state and because file creation
	 * is forbidden (in dir_s2d2).
	 */
	ASSERT_EQ(-1, renameat2(AT_FDCWD, dir_file2_s2d3, AT_FDCWD, file1_s2d2,
				RENAME_EXCHANGE));
	ASSERT_EQ(EACCES, errno);
	ASSERT_EQ(-1, renameat2(AT_FDCWD, file1_s2d2, AT_FDCWD, dir_file2_s2d3,
				RENAME_EXCHANGE));
	ASSERT_EQ(EACCES, errno);

	reparent_exdev_layers_enforce2(_metadata);

	/* Error predominance with file exchange: returns EXDEV and EACCES. */
	ASSERT_EQ(-1, renameat2(AT_FDCWD, file1_s1d1, AT_FDCWD, file1_s2d3,
				RENAME_EXCHANGE));
	ASSERT_EQ(EACCES, errno);
	ASSERT_EQ(-1, renameat2(AT_FDCWD, file1_s2d3, AT_FDCWD, file1_s1d1,
				RENAME_EXCHANGE));
	ASSERT_EQ(EACCES, errno);

	/* Checks with directories which creation is now denied. */
	ASSERT_EQ(-1, renameat2(AT_FDCWD, dir_file1_s1d2, AT_FDCWD,
				dir_file2_s2d3, RENAME_EXCHANGE));
	ASSERT_EQ(EACCES, errno);
	ASSERT_EQ(-1, renameat2(AT_FDCWD, dir_file2_s2d3, AT_FDCWD,
				dir_file1_s1d2, RENAME_EXCHANGE));
	ASSERT_EQ(EACCES, errno);

	/* Checks with different (child-only) access rights. */
	ASSERT_EQ(-1, renameat2(AT_FDCWD, dir_s1d3, AT_FDCWD, dir_s2d3,
				RENAME_EXCHANGE));
	/* Denied because of MAKE_DIR. */
	ASSERT_EQ(EACCES, errno);
	ASSERT_EQ(-1, renameat2(AT_FDCWD, dir_s2d3, AT_FDCWD, dir_s1d3,
				RENAME_EXCHANGE));
	ASSERT_EQ(EACCES, errno);

	/* Checks with different (child-only) access rights. */
	ASSERT_EQ(-1, renameat2(AT_FDCWD, dir_s2d3, AT_FDCWD, dir_file1_s1d2,
				RENAME_EXCHANGE));
	/* Denied because of MAKE_DIR. */
	ASSERT_EQ(EACCES, errno);
	ASSERT_EQ(-1, renameat2(AT_FDCWD, dir_file1_s1d2, AT_FDCWD, dir_s2d3,
				RENAME_EXCHANGE));
	ASSERT_EQ(EACCES, errno);

	/* See layout1.reparent_exdev_layers_exchange2 for complement. */
}

TEST_F_FORK(layout1, reparent_exdev_layers_exchange2)
{
	const char *const dir_file2_s2d3 = file2_s2d3;

	ASSERT_EQ(0, unlink(file2_s2d3));
	ASSERT_EQ(0, mkdir(file2_s2d3, 0700));

	reparent_exdev_layers_enforce1(_metadata);
	reparent_exdev_layers_enforce2(_metadata);

	/* Checks that exchange between file and directory are consistent. */
	ASSERT_EQ(-1, renameat2(AT_FDCWD, file1_s2d2, AT_FDCWD, dir_file2_s2d3,
				RENAME_EXCHANGE));
	ASSERT_EQ(EACCES, errno);
	ASSERT_EQ(-1, renameat2(AT_FDCWD, dir_file2_s2d3, AT_FDCWD, file1_s2d2,
				RENAME_EXCHANGE));
	ASSERT_EQ(EACCES, errno);
}

TEST_F_FORK(layout1, reparent_exdev_layers_exchange3)
{
	const char *const dir_file2_s2d3 = file2_s2d3;

	ASSERT_EQ(0, unlink(file2_s2d3));
	ASSERT_EQ(0, mkdir(file2_s2d3, 0700));

	reparent_exdev_layers_enforce1(_metadata);

	/*
	 * Checks that exchange between file and directory are consistent,
	 * including with inverted arguments (see
	 * layout1.reparent_exdev_layers_exchange1).
	 */
	ASSERT_EQ(0, renameat2(AT_FDCWD, dir_file2_s2d3, AT_FDCWD, file1_s2d2,
			       RENAME_EXCHANGE));
	ASSERT_EQ(-1, renameat2(AT_FDCWD, file1_s2d2, AT_FDCWD, dir_file2_s2d3,
				RENAME_EXCHANGE));
	ASSERT_EQ(EACCES, errno);
	ASSERT_EQ(-1, renameat2(AT_FDCWD, dir_file2_s2d3, AT_FDCWD, file1_s2d2,
				RENAME_EXCHANGE));
	ASSERT_EQ(EACCES, errno);
}

TEST_F_FORK(layout1, reparent_remove)
{
	const struct rule layer1[] = {
		{
			.path = dir_s1d1,
			.access = LANDLOCK_ACCESS_FS_REFER |
				  LANDLOCK_ACCESS_FS_REMOVE_DIR,
		},
		{
			.path = dir_s1d2,
			.access = LANDLOCK_ACCESS_FS_REMOVE_FILE,
		},
		{
			.path = dir_s2d1,
			.access = LANDLOCK_ACCESS_FS_REFER |
				  LANDLOCK_ACCESS_FS_REMOVE_FILE,
		},
		{},
	};
	const int ruleset_fd = create_ruleset(
		_metadata,
		LANDLOCK_ACCESS_FS_REFER | LANDLOCK_ACCESS_FS_REMOVE_DIR |
			LANDLOCK_ACCESS_FS_REMOVE_FILE,
		layer1);

	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/* Access denied because of wrong/swapped remove file/dir. */
	ASSERT_EQ(-1, rename(file1_s1d1, dir_s2d2));
	ASSERT_EQ(EACCES, errno);
	ASSERT_EQ(-1, rename(dir_s2d2, file1_s1d1));
	ASSERT_EQ(EACCES, errno);
	ASSERT_EQ(-1, renameat2(AT_FDCWD, file1_s1d1, AT_FDCWD, dir_s2d2,
				RENAME_EXCHANGE));
	ASSERT_EQ(EACCES, errno);
	ASSERT_EQ(-1, renameat2(AT_FDCWD, file1_s1d1, AT_FDCWD, dir_s2d3,
				RENAME_EXCHANGE));
	ASSERT_EQ(EACCES, errno);

	/* Access allowed thanks to the matching rights. */
	ASSERT_EQ(-1, rename(file1_s2d1, dir_s1d2));
	ASSERT_EQ(EISDIR, errno);
	ASSERT_EQ(-1, rename(dir_s1d2, file1_s2d1));
	ASSERT_EQ(ENOTDIR, errno);
	ASSERT_EQ(-1, rename(dir_s1d3, file1_s2d1));
	ASSERT_EQ(ENOTDIR, errno);
	ASSERT_EQ(0, unlink(file1_s2d1));
	ASSERT_EQ(0, unlink(file1_s1d3));
	ASSERT_EQ(0, unlink(file2_s1d3));
	ASSERT_EQ(0, rename(dir_s1d3, file1_s2d1));

	/* Effectively removes a file and a directory by exchanging them. */
	ASSERT_EQ(0, mkdir(dir_s1d3, 0700));
	ASSERT_EQ(0, renameat2(AT_FDCWD, file1_s2d2, AT_FDCWD, dir_s1d3,
			       RENAME_EXCHANGE));
	ASSERT_EQ(-1, renameat2(AT_FDCWD, file1_s2d2, AT_FDCWD, dir_s1d3,
				RENAME_EXCHANGE));
	ASSERT_EQ(EACCES, errno);
}

TEST_F_FORK(layout1, reparent_dom_superset)
{
	const struct rule layer1[] = {
		{
			.path = dir_s1d2,
			.access = LANDLOCK_ACCESS_FS_REFER,
		},
		{
			.path = file1_s1d2,
			.access = LANDLOCK_ACCESS_FS_EXECUTE,
		},
		{
			.path = dir_s1d3,
			.access = LANDLOCK_ACCESS_FS_MAKE_SOCK |
				  LANDLOCK_ACCESS_FS_EXECUTE,
		},
		{
			.path = dir_s2d2,
			.access = LANDLOCK_ACCESS_FS_REFER |
				  LANDLOCK_ACCESS_FS_EXECUTE |
				  LANDLOCK_ACCESS_FS_MAKE_SOCK,
		},
		{
			.path = dir_s2d3,
			.access = LANDLOCK_ACCESS_FS_READ_FILE |
				  LANDLOCK_ACCESS_FS_MAKE_FIFO,
		},
		{},
	};
	int ruleset_fd = create_ruleset(_metadata,
					LANDLOCK_ACCESS_FS_REFER |
						LANDLOCK_ACCESS_FS_EXECUTE |
						LANDLOCK_ACCESS_FS_MAKE_SOCK |
						LANDLOCK_ACCESS_FS_READ_FILE |
						LANDLOCK_ACCESS_FS_MAKE_FIFO,
					layer1);

	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	ASSERT_EQ(-1, rename(file1_s1d2, file1_s2d1));
	ASSERT_EQ(EXDEV, errno);
	/*
	 * Moving file1_s1d2 beneath dir_s2d3 would grant it the READ_FILE
	 * access right.
	 */
	ASSERT_EQ(-1, rename(file1_s1d2, file1_s2d3));
	ASSERT_EQ(EXDEV, errno);
	/*
	 * Moving file1_s1d2 should be allowed even if dir_s2d2 grants a
	 * superset of access rights compared to dir_s1d2, because file1_s1d2
	 * already has these access rights anyway.
	 */
	ASSERT_EQ(0, rename(file1_s1d2, file1_s2d2));
	ASSERT_EQ(0, rename(file1_s2d2, file1_s1d2));

	ASSERT_EQ(-1, rename(dir_s1d3, file1_s2d1));
	ASSERT_EQ(EXDEV, errno);
	/*
	 * Moving dir_s1d3 beneath dir_s2d3 would grant it the MAKE_FIFO access
	 * right.
	 */
	ASSERT_EQ(-1, rename(dir_s1d3, file1_s2d3));
	ASSERT_EQ(EXDEV, errno);
	/*
	 * Moving dir_s1d3 should be allowed even if dir_s2d2 grants a superset
	 * of access rights compared to dir_s1d2, because dir_s1d3 already has
	 * these access rights anyway.
	 */
	ASSERT_EQ(0, rename(dir_s1d3, file1_s2d2));
	ASSERT_EQ(0, rename(file1_s2d2, dir_s1d3));

	/*
	 * Moving file1_s2d3 beneath dir_s1d2 is allowed, but moving it back
	 * will be denied because the new inherited access rights from dir_s1d2
	 * will be less than the destination (original) dir_s2d3.  This is a
	 * sinkhole scenario where we cannot move back files or directories.
	 */
	ASSERT_EQ(0, rename(file1_s2d3, file2_s1d2));
	ASSERT_EQ(-1, rename(file2_s1d2, file1_s2d3));
	ASSERT_EQ(EXDEV, errno);
	ASSERT_EQ(0, unlink(file2_s1d2));
	ASSERT_EQ(0, unlink(file2_s2d3));
	/*
	 * Checks similar directory one-way move: dir_s2d3 loses EXECUTE and
	 * MAKE_SOCK which were inherited from dir_s1d3.
	 */
	ASSERT_EQ(0, rename(dir_s2d3, file2_s1d2));
	ASSERT_EQ(-1, rename(file2_s1d2, dir_s2d3));
	ASSERT_EQ(EXDEV, errno);
}

TEST_F_FORK(layout1, remove_dir)
{
	const struct rule rules[] = {
		{
			.path = dir_s1d2,
			.access = LANDLOCK_ACCESS_FS_REMOVE_DIR,
		},
		{},
	};
	const int ruleset_fd =
		create_ruleset(_metadata, rules[0].access, rules);

	ASSERT_LE(0, ruleset_fd);

	ASSERT_EQ(0, unlink(file1_s1d1));
	ASSERT_EQ(0, unlink(file1_s1d2));
	ASSERT_EQ(0, unlink(file1_s1d3));
	ASSERT_EQ(0, unlink(file2_s1d3));

	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	ASSERT_EQ(0, rmdir(dir_s1d3));
	ASSERT_EQ(0, mkdir(dir_s1d3, 0700));
	ASSERT_EQ(0, unlinkat(AT_FDCWD, dir_s1d3, AT_REMOVEDIR));

	/* dir_s1d2 itself cannot be removed. */
	ASSERT_EQ(-1, rmdir(dir_s1d2));
	ASSERT_EQ(EACCES, errno);
	ASSERT_EQ(-1, unlinkat(AT_FDCWD, dir_s1d2, AT_REMOVEDIR));
	ASSERT_EQ(EACCES, errno);
	ASSERT_EQ(-1, rmdir(dir_s1d1));
	ASSERT_EQ(EACCES, errno);
	ASSERT_EQ(-1, unlinkat(AT_FDCWD, dir_s1d1, AT_REMOVEDIR));
	ASSERT_EQ(EACCES, errno);
}

TEST_F_FORK(layout1, remove_file)
{
	const struct rule rules[] = {
		{
			.path = dir_s1d2,
			.access = LANDLOCK_ACCESS_FS_REMOVE_FILE,
		},
		{},
	};
	const int ruleset_fd =
		create_ruleset(_metadata, rules[0].access, rules);

	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	ASSERT_EQ(-1, unlink(file1_s1d1));
	ASSERT_EQ(EACCES, errno);
	ASSERT_EQ(-1, unlinkat(AT_FDCWD, file1_s1d1, 0));
	ASSERT_EQ(EACCES, errno);
	ASSERT_EQ(0, unlink(file1_s1d2));
	ASSERT_EQ(0, unlinkat(AT_FDCWD, file1_s1d3, 0));
}

static void test_make_file(struct __test_metadata *const _metadata,
			   const __u64 access, const mode_t mode,
			   const dev_t dev)
{
	const struct rule rules[] = {
		{
			.path = dir_s1d2,
			.access = access,
		},
		{},
	};
	const int ruleset_fd = create_ruleset(_metadata, access, rules);

	ASSERT_LE(0, ruleset_fd);

	ASSERT_EQ(0, unlink(file1_s1d1));
	ASSERT_EQ(0, unlink(file2_s1d1));
	ASSERT_EQ(0, mknod(file2_s1d1, mode | 0400, dev))
	{
		TH_LOG("Failed to make file \"%s\": %s", file2_s1d1,
		       strerror(errno));
	};

	ASSERT_EQ(0, unlink(file1_s1d2));
	ASSERT_EQ(0, unlink(file2_s1d2));

	ASSERT_EQ(0, unlink(file1_s1d3));
	ASSERT_EQ(0, unlink(file2_s1d3));

	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	ASSERT_EQ(-1, mknod(file1_s1d1, mode | 0400, dev));
	ASSERT_EQ(EACCES, errno);
	ASSERT_EQ(-1, link(file2_s1d1, file1_s1d1));
	ASSERT_EQ(EACCES, errno);
	ASSERT_EQ(-1, rename(file2_s1d1, file1_s1d1));
	ASSERT_EQ(EACCES, errno);

	ASSERT_EQ(0, mknod(file1_s1d2, mode | 0400, dev))
	{
		TH_LOG("Failed to make file \"%s\": %s", file1_s1d2,
		       strerror(errno));
	};
	ASSERT_EQ(0, link(file1_s1d2, file2_s1d2));
	ASSERT_EQ(0, unlink(file2_s1d2));
	ASSERT_EQ(0, rename(file1_s1d2, file2_s1d2));

	ASSERT_EQ(0, mknod(file1_s1d3, mode | 0400, dev));
	ASSERT_EQ(0, link(file1_s1d3, file2_s1d3));
	ASSERT_EQ(0, unlink(file2_s1d3));
	ASSERT_EQ(0, rename(file1_s1d3, file2_s1d3));
}

TEST_F_FORK(layout1, make_char)
{
	/* Creates a /dev/null device. */
	set_cap(_metadata, CAP_MKNOD);
	test_make_file(_metadata, LANDLOCK_ACCESS_FS_MAKE_CHAR, S_IFCHR,
		       makedev(1, 3));
}

TEST_F_FORK(layout1, make_block)
{
	/* Creates a /dev/loop0 device. */
	set_cap(_metadata, CAP_MKNOD);
	test_make_file(_metadata, LANDLOCK_ACCESS_FS_MAKE_BLOCK, S_IFBLK,
		       makedev(7, 0));
}

TEST_F_FORK(layout1, make_reg_1)
{
	test_make_file(_metadata, LANDLOCK_ACCESS_FS_MAKE_REG, S_IFREG, 0);
}

TEST_F_FORK(layout1, make_reg_2)
{
	test_make_file(_metadata, LANDLOCK_ACCESS_FS_MAKE_REG, 0, 0);
}

TEST_F_FORK(layout1, make_sock)
{
	test_make_file(_metadata, LANDLOCK_ACCESS_FS_MAKE_SOCK, S_IFSOCK, 0);
}

TEST_F_FORK(layout1, make_fifo)
{
	test_make_file(_metadata, LANDLOCK_ACCESS_FS_MAKE_FIFO, S_IFIFO, 0);
}

TEST_F_FORK(layout1, make_sym)
{
	const struct rule rules[] = {
		{
			.path = dir_s1d2,
			.access = LANDLOCK_ACCESS_FS_MAKE_SYM,
		},
		{},
	};
	const int ruleset_fd =
		create_ruleset(_metadata, rules[0].access, rules);

	ASSERT_LE(0, ruleset_fd);

	ASSERT_EQ(0, unlink(file1_s1d1));
	ASSERT_EQ(0, unlink(file2_s1d1));
	ASSERT_EQ(0, symlink("none", file2_s1d1));

	ASSERT_EQ(0, unlink(file1_s1d2));
	ASSERT_EQ(0, unlink(file2_s1d2));

	ASSERT_EQ(0, unlink(file1_s1d3));
	ASSERT_EQ(0, unlink(file2_s1d3));

	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	ASSERT_EQ(-1, symlink("none", file1_s1d1));
	ASSERT_EQ(EACCES, errno);
	ASSERT_EQ(-1, link(file2_s1d1, file1_s1d1));
	ASSERT_EQ(EACCES, errno);
	ASSERT_EQ(-1, rename(file2_s1d1, file1_s1d1));
	ASSERT_EQ(EACCES, errno);

	ASSERT_EQ(0, symlink("none", file1_s1d2));
	ASSERT_EQ(0, link(file1_s1d2, file2_s1d2));
	ASSERT_EQ(0, unlink(file2_s1d2));
	ASSERT_EQ(0, rename(file1_s1d2, file2_s1d2));

	ASSERT_EQ(0, symlink("none", file1_s1d3));
	ASSERT_EQ(0, link(file1_s1d3, file2_s1d3));
	ASSERT_EQ(0, unlink(file2_s1d3));
	ASSERT_EQ(0, rename(file1_s1d3, file2_s1d3));
}

TEST_F_FORK(layout1, make_dir)
{
	const struct rule rules[] = {
		{
			.path = dir_s1d2,
			.access = LANDLOCK_ACCESS_FS_MAKE_DIR,
		},
		{},
	};
	const int ruleset_fd =
		create_ruleset(_metadata, rules[0].access, rules);

	ASSERT_LE(0, ruleset_fd);

	ASSERT_EQ(0, unlink(file1_s1d1));
	ASSERT_EQ(0, unlink(file1_s1d2));
	ASSERT_EQ(0, unlink(file1_s1d3));

	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/* Uses file_* as directory names. */
	ASSERT_EQ(-1, mkdir(file1_s1d1, 0700));
	ASSERT_EQ(EACCES, errno);
	ASSERT_EQ(0, mkdir(file1_s1d2, 0700));
	ASSERT_EQ(0, mkdir(file1_s1d3, 0700));
}

static int open_proc_fd(struct __test_metadata *const _metadata, const int fd,
			const int open_flags)
{
	static const char path_template[] = "/proc/self/fd/%d";
	char procfd_path[sizeof(path_template) + 10];
	const int procfd_path_size =
		snprintf(procfd_path, sizeof(procfd_path), path_template, fd);

	ASSERT_LT(procfd_path_size, sizeof(procfd_path));
	return open(procfd_path, open_flags);
}

TEST_F_FORK(layout1, proc_unlinked_file)
{
	const struct rule rules[] = {
		{
			.path = file1_s1d2,
			.access = LANDLOCK_ACCESS_FS_READ_FILE,
		},
		{},
	};
	int reg_fd, proc_fd;
	const int ruleset_fd = create_ruleset(
		_metadata,
		LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE,
		rules);

	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	ASSERT_EQ(EACCES, test_open(file1_s1d2, O_RDWR));
	ASSERT_EQ(0, test_open(file1_s1d2, O_RDONLY));
	reg_fd = open(file1_s1d2, O_RDONLY | O_CLOEXEC);
	ASSERT_LE(0, reg_fd);
	ASSERT_EQ(0, unlink(file1_s1d2));

	proc_fd = open_proc_fd(_metadata, reg_fd, O_RDONLY | O_CLOEXEC);
	ASSERT_LE(0, proc_fd);
	ASSERT_EQ(0, close(proc_fd));

	proc_fd = open_proc_fd(_metadata, reg_fd, O_RDWR | O_CLOEXEC);
	ASSERT_EQ(-1, proc_fd)
	{
		TH_LOG("Successfully opened /proc/self/fd/%d: %s", reg_fd,
		       strerror(errno));
	}
	ASSERT_EQ(EACCES, errno);

	ASSERT_EQ(0, close(reg_fd));
}

TEST_F_FORK(layout1, proc_pipe)
{
	int proc_fd;
	int pipe_fds[2];
	char buf = '\0';
	const struct rule rules[] = {
		{
			.path = dir_s1d2,
			.access = LANDLOCK_ACCESS_FS_READ_FILE |
				  LANDLOCK_ACCESS_FS_WRITE_FILE,
		},
		{},
	};
	/* Limits read and write access to files tied to the filesystem. */
	const int ruleset_fd =
		create_ruleset(_metadata, rules[0].access, rules);

	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/* Checks enforcement for normal files. */
	ASSERT_EQ(0, test_open(file1_s1d2, O_RDWR));
	ASSERT_EQ(EACCES, test_open(file1_s1d1, O_RDWR));

	/* Checks access to pipes through FD. */
	ASSERT_EQ(0, pipe2(pipe_fds, O_CLOEXEC));
	ASSERT_EQ(1, write(pipe_fds[1], ".", 1))
	{
		TH_LOG("Failed to write in pipe: %s", strerror(errno));
	}
	ASSERT_EQ(1, read(pipe_fds[0], &buf, 1));
	ASSERT_EQ('.', buf);

	/* Checks write access to pipe through /proc/self/fd . */
	proc_fd = open_proc_fd(_metadata, pipe_fds[1], O_WRONLY | O_CLOEXEC);
	ASSERT_LE(0, proc_fd);
	ASSERT_EQ(1, write(proc_fd, ".", 1))
	{
		TH_LOG("Failed to write through /proc/self/fd/%d: %s",
		       pipe_fds[1], strerror(errno));
	}
	ASSERT_EQ(0, close(proc_fd));

	/* Checks read access to pipe through /proc/self/fd . */
	proc_fd = open_proc_fd(_metadata, pipe_fds[0], O_RDONLY | O_CLOEXEC);
	ASSERT_LE(0, proc_fd);
	buf = '\0';
	ASSERT_EQ(1, read(proc_fd, &buf, 1))
	{
		TH_LOG("Failed to read through /proc/self/fd/%d: %s",
		       pipe_fds[1], strerror(errno));
	}
	ASSERT_EQ(0, close(proc_fd));

	ASSERT_EQ(0, close(pipe_fds[0]));
	ASSERT_EQ(0, close(pipe_fds[1]));
}

/* Invokes truncate(2) and returns its errno or 0. */
static int test_truncate(const char *const path)
{
	if (truncate(path, 10) < 0)
		return errno;
	return 0;
}

/*
 * Invokes creat(2) and returns its errno or 0.
 * Closes the opened file descriptor on success.
 */
static int test_creat(const char *const path)
{
	int fd = creat(path, 0600);

	if (fd < 0)
		return errno;

	/*
	 * Mixing error codes from close(2) and creat(2) should not lead to any
	 * (access type) confusion for this test.
	 */
	if (close(fd) < 0)
		return errno;
	return 0;
}

/*
 * Exercises file truncation when it's not restricted,
 * as it was the case before LANDLOCK_ACCESS_FS_TRUNCATE existed.
 */
TEST_F_FORK(layout1, truncate_unhandled)
{
	const char *const file_r = file1_s1d1;
	const char *const file_w = file2_s1d1;
	const char *const file_none = file1_s1d2;
	const struct rule rules[] = {
		{
			.path = file_r,
			.access = LANDLOCK_ACCESS_FS_READ_FILE,
		},
		{
			.path = file_w,
			.access = LANDLOCK_ACCESS_FS_WRITE_FILE,
		},
		/* Implicitly: No rights for file_none. */
		{},
	};

	const __u64 handled = LANDLOCK_ACCESS_FS_READ_FILE |
			      LANDLOCK_ACCESS_FS_WRITE_FILE;
	int ruleset_fd;

	/* Enables Landlock. */
	ruleset_fd = create_ruleset(_metadata, handled, rules);

	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/*
	 * Checks read right: truncate and open with O_TRUNC work, unless the
	 * file is attempted to be opened for writing.
	 */
	EXPECT_EQ(0, test_truncate(file_r));
	EXPECT_EQ(0, test_open(file_r, O_RDONLY | O_TRUNC));
	EXPECT_EQ(EACCES, test_open(file_r, O_WRONLY | O_TRUNC));
	EXPECT_EQ(EACCES, test_creat(file_r));

	/*
	 * Checks write right: truncate and open with O_TRUNC work, unless the
	 * file is attempted to be opened for reading.
	 */
	EXPECT_EQ(0, test_truncate(file_w));
	EXPECT_EQ(EACCES, test_open(file_w, O_RDONLY | O_TRUNC));
	EXPECT_EQ(0, test_open(file_w, O_WRONLY | O_TRUNC));
	EXPECT_EQ(0, test_creat(file_w));

	/*
	 * Checks "no rights" case: truncate works but all open attempts fail,
	 * including creat.
	 */
	EXPECT_EQ(0, test_truncate(file_none));
	EXPECT_EQ(EACCES, test_open(file_none, O_RDONLY | O_TRUNC));
	EXPECT_EQ(EACCES, test_open(file_none, O_WRONLY | O_TRUNC));
	EXPECT_EQ(EACCES, test_creat(file_none));
}

TEST_F_FORK(layout1, truncate)
{
	const char *const file_rwt = file1_s1d1;
	const char *const file_rw = file2_s1d1;
	const char *const file_rt = file1_s1d2;
	const char *const file_t = file2_s1d2;
	const char *const file_none = file1_s1d3;
	const char *const dir_t = dir_s2d1;
	const char *const file_in_dir_t = file1_s2d1;
	const char *const dir_w = dir_s3d1;
	const char *const file_in_dir_w = file1_s3d1;
	const struct rule rules[] = {
		{
			.path = file_rwt,
			.access = LANDLOCK_ACCESS_FS_READ_FILE |
				  LANDLOCK_ACCESS_FS_WRITE_FILE |
				  LANDLOCK_ACCESS_FS_TRUNCATE,
		},
		{
			.path = file_rw,
			.access = LANDLOCK_ACCESS_FS_READ_FILE |
				  LANDLOCK_ACCESS_FS_WRITE_FILE,
		},
		{
			.path = file_rt,
			.access = LANDLOCK_ACCESS_FS_READ_FILE |
				  LANDLOCK_ACCESS_FS_TRUNCATE,
		},
		{
			.path = file_t,
			.access = LANDLOCK_ACCESS_FS_TRUNCATE,
		},
		/* Implicitly: No access rights for file_none. */
		{
			.path = dir_t,
			.access = LANDLOCK_ACCESS_FS_TRUNCATE,
		},
		{
			.path = dir_w,
			.access = LANDLOCK_ACCESS_FS_WRITE_FILE,
		},
		{},
	};
	const __u64 handled = LANDLOCK_ACCESS_FS_READ_FILE |
			      LANDLOCK_ACCESS_FS_WRITE_FILE |
			      LANDLOCK_ACCESS_FS_TRUNCATE;
	int ruleset_fd;

	/* Enables Landlock. */
	ruleset_fd = create_ruleset(_metadata, handled, rules);

	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/* Checks read, write and truncate rights: truncation works. */
	EXPECT_EQ(0, test_truncate(file_rwt));
	EXPECT_EQ(0, test_open(file_rwt, O_RDONLY | O_TRUNC));
	EXPECT_EQ(0, test_open(file_rwt, O_WRONLY | O_TRUNC));

	/* Checks read and write rights: no truncate variant works. */
	EXPECT_EQ(EACCES, test_truncate(file_rw));
	EXPECT_EQ(EACCES, test_open(file_rw, O_RDONLY | O_TRUNC));
	EXPECT_EQ(EACCES, test_open(file_rw, O_WRONLY | O_TRUNC));

	/*
	 * Checks read and truncate rights: truncation works.
	 *
	 * Note: Files can get truncated using open() even with O_RDONLY.
	 */
	EXPECT_EQ(0, test_truncate(file_rt));
	EXPECT_EQ(0, test_open(file_rt, O_RDONLY | O_TRUNC));
	EXPECT_EQ(EACCES, test_open(file_rt, O_WRONLY | O_TRUNC));

	/* Checks truncate right: truncate works, but can't open file. */
	EXPECT_EQ(0, test_truncate(file_t));
	EXPECT_EQ(EACCES, test_open(file_t, O_RDONLY | O_TRUNC));
	EXPECT_EQ(EACCES, test_open(file_t, O_WRONLY | O_TRUNC));

	/* Checks "no rights" case: No form of truncation works. */
	EXPECT_EQ(EACCES, test_truncate(file_none));
	EXPECT_EQ(EACCES, test_open(file_none, O_RDONLY | O_TRUNC));
	EXPECT_EQ(EACCES, test_open(file_none, O_WRONLY | O_TRUNC));

	/*
	 * Checks truncate right on directory: truncate works on contained
	 * files.
	 */
	EXPECT_EQ(0, test_truncate(file_in_dir_t));
	EXPECT_EQ(EACCES, test_open(file_in_dir_t, O_RDONLY | O_TRUNC));
	EXPECT_EQ(EACCES, test_open(file_in_dir_t, O_WRONLY | O_TRUNC));

	/*
	 * Checks creat in dir_w: This requires the truncate right when
	 * overwriting an existing file, but does not require it when the file
	 * is new.
	 */
	EXPECT_EQ(EACCES, test_creat(file_in_dir_w));

	ASSERT_EQ(0, unlink(file_in_dir_w));
	EXPECT_EQ(0, test_creat(file_in_dir_w));
}

/* Invokes ftruncate(2) and returns its errno or 0. */
static int test_ftruncate(int fd)
{
	if (ftruncate(fd, 10) < 0)
		return errno;
	return 0;
}

TEST_F_FORK(layout1, ftruncate)
{
	/*
	 * This test opens a new file descriptor at different stages of
	 * Landlock restriction:
	 *
	 * without restriction:                    ftruncate works
	 * something else but truncate restricted: ftruncate works
	 * truncate restricted and permitted:      ftruncate works
	 * truncate restricted and not permitted:  ftruncate fails
	 *
	 * Whether this works or not is expected to depend on the time when the
	 * FD was opened, not to depend on the time when ftruncate() was
	 * called.
	 */
	const char *const path = file1_s1d1;
	const __u64 handled1 = LANDLOCK_ACCESS_FS_READ_FILE |
			       LANDLOCK_ACCESS_FS_WRITE_FILE;
	const struct rule layer1[] = {
		{
			.path = path,
			.access = LANDLOCK_ACCESS_FS_WRITE_FILE,
		},
		{},
	};
	const __u64 handled2 = LANDLOCK_ACCESS_FS_TRUNCATE;
	const struct rule layer2[] = {
		{
			.path = path,
			.access = LANDLOCK_ACCESS_FS_TRUNCATE,
		},
		{},
	};
	const __u64 handled3 = LANDLOCK_ACCESS_FS_TRUNCATE |
			       LANDLOCK_ACCESS_FS_WRITE_FILE;
	const struct rule layer3[] = {
		{
			.path = path,
			.access = LANDLOCK_ACCESS_FS_WRITE_FILE,
		},
		{},
	};
	int fd_layer0, fd_layer1, fd_layer2, fd_layer3, ruleset_fd;

	fd_layer0 = open(path, O_WRONLY);
	EXPECT_EQ(0, test_ftruncate(fd_layer0));

	ruleset_fd = create_ruleset(_metadata, handled1, layer1);
	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	fd_layer1 = open(path, O_WRONLY);
	EXPECT_EQ(0, test_ftruncate(fd_layer0));
	EXPECT_EQ(0, test_ftruncate(fd_layer1));

	ruleset_fd = create_ruleset(_metadata, handled2, layer2);
	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	fd_layer2 = open(path, O_WRONLY);
	EXPECT_EQ(0, test_ftruncate(fd_layer0));
	EXPECT_EQ(0, test_ftruncate(fd_layer1));
	EXPECT_EQ(0, test_ftruncate(fd_layer2));

	ruleset_fd = create_ruleset(_metadata, handled3, layer3);
	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	fd_layer3 = open(path, O_WRONLY);
	EXPECT_EQ(0, test_ftruncate(fd_layer0));
	EXPECT_EQ(0, test_ftruncate(fd_layer1));
	EXPECT_EQ(0, test_ftruncate(fd_layer2));
	EXPECT_EQ(EACCES, test_ftruncate(fd_layer3));

	ASSERT_EQ(0, close(fd_layer0));
	ASSERT_EQ(0, close(fd_layer1));
	ASSERT_EQ(0, close(fd_layer2));
	ASSERT_EQ(0, close(fd_layer3));
}

/* clang-format off */
FIXTURE(ftruncate) {};
/* clang-format on */

FIXTURE_SETUP(ftruncate)
{
	prepare_layout(_metadata);
	create_file(_metadata, file1_s1d1);
}

FIXTURE_TEARDOWN_PARENT(ftruncate)
{
	EXPECT_EQ(0, remove_path(file1_s1d1));
	cleanup_layout(_metadata);
}

FIXTURE_VARIANT(ftruncate)
{
	const __u64 handled;
	const __u64 allowed;
	const int expected_open_result;
	const int expected_ftruncate_result;
};

/* clang-format off */
FIXTURE_VARIANT_ADD(ftruncate, w_w) {
	/* clang-format on */
	.handled = LANDLOCK_ACCESS_FS_WRITE_FILE,
	.allowed = LANDLOCK_ACCESS_FS_WRITE_FILE,
	.expected_open_result = 0,
	.expected_ftruncate_result = 0,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(ftruncate, t_t) {
	/* clang-format on */
	.handled = LANDLOCK_ACCESS_FS_TRUNCATE,
	.allowed = LANDLOCK_ACCESS_FS_TRUNCATE,
	.expected_open_result = 0,
	.expected_ftruncate_result = 0,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(ftruncate, wt_w) {
	/* clang-format on */
	.handled = LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_TRUNCATE,
	.allowed = LANDLOCK_ACCESS_FS_WRITE_FILE,
	.expected_open_result = 0,
	.expected_ftruncate_result = EACCES,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(ftruncate, wt_wt) {
	/* clang-format on */
	.handled = LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_TRUNCATE,
	.allowed = LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_TRUNCATE,
	.expected_open_result = 0,
	.expected_ftruncate_result = 0,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(ftruncate, wt_t) {
	/* clang-format on */
	.handled = LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_TRUNCATE,
	.allowed = LANDLOCK_ACCESS_FS_TRUNCATE,
	.expected_open_result = EACCES,
};

TEST_F_FORK(ftruncate, open_and_ftruncate)
{
	const char *const path = file1_s1d1;
	const struct rule rules[] = {
		{
			.path = path,
			.access = variant->allowed,
		},
		{},
	};
	int fd, ruleset_fd;

	/* Enables Landlock. */
	ruleset_fd = create_ruleset(_metadata, variant->handled, rules);
	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	fd = open(path, O_WRONLY);
	EXPECT_EQ(variant->expected_open_result, (fd < 0 ? errno : 0));
	if (fd >= 0) {
		EXPECT_EQ(variant->expected_ftruncate_result,
			  test_ftruncate(fd));
		ASSERT_EQ(0, close(fd));
	}
}

TEST_F_FORK(ftruncate, open_and_ftruncate_in_different_processes)
{
	int child, fd, status;
	int socket_fds[2];

	ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0,
				socket_fds));

	child = fork();
	ASSERT_LE(0, child);
	if (child == 0) {
		/*
		 * Enables Landlock in the child process, open a file descriptor
		 * where truncation is forbidden and send it to the
		 * non-landlocked parent process.
		 */
		const char *const path = file1_s1d1;
		const struct rule rules[] = {
			{
				.path = path,
				.access = variant->allowed,
			},
			{},
		};
		int fd, ruleset_fd;

		ruleset_fd = create_ruleset(_metadata, variant->handled, rules);
		ASSERT_LE(0, ruleset_fd);
		enforce_ruleset(_metadata, ruleset_fd);
		ASSERT_EQ(0, close(ruleset_fd));

		fd = open(path, O_WRONLY);
		ASSERT_EQ(variant->expected_open_result, (fd < 0 ? errno : 0));

		if (fd >= 0) {
			ASSERT_EQ(0, send_fd(socket_fds[0], fd));
			ASSERT_EQ(0, close(fd));
		}

		ASSERT_EQ(0, close(socket_fds[0]));

		_exit(_metadata->exit_code);
		return;
	}

	if (variant->expected_open_result == 0) {
		fd = recv_fd(socket_fds[1]);
		ASSERT_LE(0, fd);

		EXPECT_EQ(variant->expected_ftruncate_result,
			  test_ftruncate(fd));
		ASSERT_EQ(0, close(fd));
	}

	ASSERT_EQ(child, waitpid(child, &status, 0));
	ASSERT_EQ(1, WIFEXITED(status));
	ASSERT_EQ(EXIT_SUCCESS, WEXITSTATUS(status));

	ASSERT_EQ(0, close(socket_fds[0]));
	ASSERT_EQ(0, close(socket_fds[1]));
}

/* Invokes the FS_IOC_GETFLAGS IOCTL and returns its errno or 0. */
static int test_fs_ioc_getflags_ioctl(int fd)
{
	uint32_t flags;

	if (ioctl(fd, FS_IOC_GETFLAGS, &flags) < 0)
		return errno;
	return 0;
}

TEST(memfd_ftruncate_and_ioctl)
{
	const struct landlock_ruleset_attr attr = {
		.handled_access_fs = ACCESS_ALL,
	};
	int ruleset_fd, fd, i;

	/*
	 * We exercise the same test both with and without Landlock enabled, to
	 * ensure that it behaves the same in both cases.
	 */
	for (i = 0; i < 2; i++) {
		/* Creates a new memfd. */
		fd = memfd_create("name", MFD_CLOEXEC);
		ASSERT_LE(0, fd);

		/*
		 * Checks that operations associated with the opened file
		 * (ftruncate, ioctl) are permitted on file descriptors that are
		 * created in ways other than open(2).
		 */
		EXPECT_EQ(0, test_ftruncate(fd));
		EXPECT_EQ(0, test_fs_ioc_getflags_ioctl(fd));

		ASSERT_EQ(0, close(fd));

		/* Enables Landlock. */
		ruleset_fd = landlock_create_ruleset(&attr, sizeof(attr), 0);
		ASSERT_LE(0, ruleset_fd);
		enforce_ruleset(_metadata, ruleset_fd);
		ASSERT_EQ(0, close(ruleset_fd));
	}
}

static int test_fionread_ioctl(int fd)
{
	size_t sz = 0;

	if (ioctl(fd, FIONREAD, &sz) < 0 && errno == EACCES)
		return errno;
	return 0;
}

TEST_F_FORK(layout1, o_path_ftruncate_and_ioctl)
{
	const struct landlock_ruleset_attr attr = {
		.handled_access_fs = ACCESS_ALL,
	};
	int ruleset_fd, fd;

	/*
	 * Checks that for files opened with O_PATH, both ioctl(2) and
	 * ftruncate(2) yield EBADF, as it is documented in open(2) for the
	 * O_PATH flag.
	 */
	fd = open(dir_s1d1, O_PATH | O_CLOEXEC);
	ASSERT_LE(0, fd);

	EXPECT_EQ(EBADF, test_ftruncate(fd));
	EXPECT_EQ(EBADF, test_fs_ioc_getflags_ioctl(fd));

	ASSERT_EQ(0, close(fd));

	/* Enables Landlock. */
	ruleset_fd = landlock_create_ruleset(&attr, sizeof(attr), 0);
	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/*
	 * Checks that after enabling Landlock,
	 * - the file can still be opened with O_PATH
	 * - both ioctl and truncate still yield EBADF (not EACCES).
	 */
	fd = open(dir_s1d1, O_PATH | O_CLOEXEC);
	ASSERT_LE(0, fd);

	EXPECT_EQ(EBADF, test_ftruncate(fd));
	EXPECT_EQ(EBADF, test_fs_ioc_getflags_ioctl(fd));

	ASSERT_EQ(0, close(fd));
}

/*
 * ioctl_error - generically call the given ioctl with a pointer to a
 * sufficiently large zeroed-out memory region.
 *
 * Returns the IOCTLs error, or 0.
 */
static int ioctl_error(struct __test_metadata *const _metadata, int fd,
		       unsigned int cmd)
{
	char buf[128]; /* sufficiently large */
	int res, stdinbak_fd;

	/*
	 * Depending on the IOCTL command, parts of the zeroed-out buffer might
	 * be interpreted as file descriptor numbers.  We do not want to
	 * accidentally operate on file descriptor 0 (stdin), so we temporarily
	 * move stdin to a different FD and close FD 0 for the IOCTL call.
	 */
	stdinbak_fd = dup(0);
	ASSERT_LT(0, stdinbak_fd);
	ASSERT_EQ(0, close(0));

	/* Invokes the IOCTL with a zeroed-out buffer. */
	bzero(&buf, sizeof(buf));
	res = ioctl(fd, cmd, &buf);

	/* Restores the old FD 0 and closes the backup FD. */
	ASSERT_EQ(0, dup2(stdinbak_fd, 0));
	ASSERT_EQ(0, close(stdinbak_fd));

	if (res < 0)
		return errno;

	return 0;
}

/* Define some linux/falloc.h IOCTL commands which are not available in uapi headers. */
struct space_resv {
	__s16 l_type;
	__s16 l_whence;
	__s64 l_start;
	__s64 l_len; /* len == 0 means until end of file */
	__s32 l_sysid;
	__u32 l_pid;
	__s32 l_pad[4]; /* reserved area */
};

#define FS_IOC_RESVSP _IOW('X', 40, struct space_resv)
#define FS_IOC_UNRESVSP _IOW('X', 41, struct space_resv)
#define FS_IOC_RESVSP64 _IOW('X', 42, struct space_resv)
#define FS_IOC_UNRESVSP64 _IOW('X', 43, struct space_resv)
#define FS_IOC_ZERO_RANGE _IOW('X', 57, struct space_resv)

/*
 * Tests a series of blanket-permitted and denied IOCTLs.
 */
TEST_F_FORK(layout1, blanket_permitted_ioctls)
{
	const struct landlock_ruleset_attr attr = {
		.handled_access_fs = LANDLOCK_ACCESS_FS_IOCTL_DEV,
	};
	int ruleset_fd, fd;

	/* Enables Landlock. */
	ruleset_fd = landlock_create_ruleset(&attr, sizeof(attr), 0);
	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	fd = open("/dev/null", O_RDWR | O_CLOEXEC);
	ASSERT_LE(0, fd);

	/*
	 * Checks permitted commands.
	 * These ones may return errors, but should not be blocked by Landlock.
	 */
	EXPECT_NE(EACCES, ioctl_error(_metadata, fd, FIOCLEX));
	EXPECT_NE(EACCES, ioctl_error(_metadata, fd, FIONCLEX));
	EXPECT_NE(EACCES, ioctl_error(_metadata, fd, FIONBIO));
	EXPECT_NE(EACCES, ioctl_error(_metadata, fd, FIOASYNC));
	EXPECT_NE(EACCES, ioctl_error(_metadata, fd, FIOQSIZE));
	EXPECT_NE(EACCES, ioctl_error(_metadata, fd, FIFREEZE));
	EXPECT_NE(EACCES, ioctl_error(_metadata, fd, FITHAW));
	EXPECT_NE(EACCES, ioctl_error(_metadata, fd, FS_IOC_FIEMAP));
	EXPECT_NE(EACCES, ioctl_error(_metadata, fd, FIGETBSZ));
	EXPECT_NE(EACCES, ioctl_error(_metadata, fd, FICLONE));
	EXPECT_NE(EACCES, ioctl_error(_metadata, fd, FICLONERANGE));
	EXPECT_NE(EACCES, ioctl_error(_metadata, fd, FIDEDUPERANGE));
	EXPECT_NE(EACCES, ioctl_error(_metadata, fd, FS_IOC_GETFSUUID));
	EXPECT_NE(EACCES, ioctl_error(_metadata, fd, FS_IOC_GETFSSYSFSPATH));

	/*
	 * Checks blocked commands.
	 * A call to a blocked IOCTL command always returns EACCES.
	 */
	EXPECT_EQ(EACCES, ioctl_error(_metadata, fd, FIONREAD));
	EXPECT_EQ(EACCES, ioctl_error(_metadata, fd, FS_IOC_GETFLAGS));
	EXPECT_EQ(EACCES, ioctl_error(_metadata, fd, FS_IOC_SETFLAGS));
	EXPECT_EQ(EACCES, ioctl_error(_metadata, fd, FS_IOC_FSGETXATTR));
	EXPECT_EQ(EACCES, ioctl_error(_metadata, fd, FS_IOC_FSSETXATTR));
	EXPECT_EQ(EACCES, ioctl_error(_metadata, fd, FIBMAP));
	EXPECT_EQ(EACCES, ioctl_error(_metadata, fd, FS_IOC_RESVSP));
	EXPECT_EQ(EACCES, ioctl_error(_metadata, fd, FS_IOC_RESVSP64));
	EXPECT_EQ(EACCES, ioctl_error(_metadata, fd, FS_IOC_UNRESVSP));
	EXPECT_EQ(EACCES, ioctl_error(_metadata, fd, FS_IOC_UNRESVSP64));
	EXPECT_EQ(EACCES, ioctl_error(_metadata, fd, FS_IOC_ZERO_RANGE));

	/* Default case is also blocked. */
	EXPECT_EQ(EACCES, ioctl_error(_metadata, fd, 0xc00ffeee));

	ASSERT_EQ(0, close(fd));
}

/*
 * Named pipes are not governed by the LANDLOCK_ACCESS_FS_IOCTL_DEV right,
 * because they are not character or block devices.
 */
TEST_F_FORK(layout1, named_pipe_ioctl)
{
	pid_t child_pid;
	int fd, ruleset_fd;
	const char *const path = file1_s1d1;
	const struct landlock_ruleset_attr attr = {
		.handled_access_fs = LANDLOCK_ACCESS_FS_IOCTL_DEV,
	};

	ASSERT_EQ(0, unlink(path));
	ASSERT_EQ(0, mkfifo(path, 0600));

	/* Enables Landlock. */
	ruleset_fd = landlock_create_ruleset(&attr, sizeof(attr), 0);
	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/* The child process opens the pipe for writing. */
	child_pid = fork();
	ASSERT_NE(-1, child_pid);
	if (child_pid == 0) {
		fd = open(path, O_WRONLY);
		close(fd);
		exit(0);
	}

	fd = open(path, O_RDONLY);
	ASSERT_LE(0, fd);

	/* FIONREAD is implemented by pipefifo_fops. */
	EXPECT_EQ(0, test_fionread_ioctl(fd));

	ASSERT_EQ(0, close(fd));
	ASSERT_EQ(0, unlink(path));

	ASSERT_EQ(child_pid, waitpid(child_pid, NULL, 0));
}

/* For named UNIX domain sockets, no IOCTL restrictions apply. */
TEST_F_FORK(layout1, named_unix_domain_socket_ioctl)
{
	const char *const path = file1_s1d1;
	int srv_fd, cli_fd, ruleset_fd;
	socklen_t size;
	struct sockaddr_un srv_un, cli_un;
	const struct landlock_ruleset_attr attr = {
		.handled_access_fs = LANDLOCK_ACCESS_FS_IOCTL_DEV,
	};

	/* Sets up a server */
	srv_un.sun_family = AF_UNIX;
	strncpy(srv_un.sun_path, path, sizeof(srv_un.sun_path));

	ASSERT_EQ(0, unlink(path));
	srv_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	ASSERT_LE(0, srv_fd);

	size = offsetof(struct sockaddr_un, sun_path) + strlen(srv_un.sun_path);
	ASSERT_EQ(0, bind(srv_fd, (struct sockaddr *)&srv_un, size));
	ASSERT_EQ(0, listen(srv_fd, 10 /* qlen */));

	/* Enables Landlock. */
	ruleset_fd = landlock_create_ruleset(&attr, sizeof(attr), 0);
	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/* Sets up a client connection to it */
	cli_un.sun_family = AF_UNIX;
	cli_fd = socket(AF_UNIX, SOCK_STREAM, 0);
	ASSERT_LE(0, cli_fd);

	size = offsetof(struct sockaddr_un, sun_path) + strlen(cli_un.sun_path);
	ASSERT_EQ(0, bind(cli_fd, (struct sockaddr *)&cli_un, size));

	bzero(&cli_un, sizeof(cli_un));
	cli_un.sun_family = AF_UNIX;
	strncpy(cli_un.sun_path, path, sizeof(cli_un.sun_path));
	size = offsetof(struct sockaddr_un, sun_path) + strlen(cli_un.sun_path);

	ASSERT_EQ(0, connect(cli_fd, (struct sockaddr *)&cli_un, size));

	/* FIONREAD and other IOCTLs should not be forbidden. */
	EXPECT_EQ(0, test_fionread_ioctl(cli_fd));

	ASSERT_EQ(0, close(cli_fd));
}

/* clang-format off */
FIXTURE(ioctl) {};

FIXTURE_SETUP(ioctl) {};

FIXTURE_TEARDOWN(ioctl) {};
/* clang-format on */

FIXTURE_VARIANT(ioctl)
{
	const __u64 handled;
	const __u64 allowed;
	const mode_t open_mode;
	/*
	 * FIONREAD is used as a characteristic device-specific IOCTL command.
	 * It is implemented in fs/ioctl.c for regular files,
	 * but we do not blanket-permit it for devices.
	 */
	const int expected_fionread_result;
};

/* clang-format off */
FIXTURE_VARIANT_ADD(ioctl, handled_i_allowed_none) {
	/* clang-format on */
	.handled = LANDLOCK_ACCESS_FS_IOCTL_DEV,
	.allowed = 0,
	.open_mode = O_RDWR,
	.expected_fionread_result = EACCES,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(ioctl, handled_i_allowed_i) {
	/* clang-format on */
	.handled = LANDLOCK_ACCESS_FS_IOCTL_DEV,
	.allowed = LANDLOCK_ACCESS_FS_IOCTL_DEV,
	.open_mode = O_RDWR,
	.expected_fionread_result = 0,
};

/* clang-format off */
FIXTURE_VARIANT_ADD(ioctl, unhandled) {
	/* clang-format on */
	.handled = LANDLOCK_ACCESS_FS_EXECUTE,
	.allowed = LANDLOCK_ACCESS_FS_EXECUTE,
	.open_mode = O_RDWR,
	.expected_fionread_result = 0,
};

TEST_F_FORK(ioctl, handle_dir_access_file)
{
	const int flag = 0;
	const struct rule rules[] = {
		{
			.path = "/dev",
			.access = variant->allowed,
		},
		{},
	};
	int file_fd, ruleset_fd;

	/* Enables Landlock. */
	ruleset_fd = create_ruleset(_metadata, variant->handled, rules);
	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	file_fd = open("/dev/zero", variant->open_mode);
	ASSERT_LE(0, file_fd);

	/* Checks that IOCTL commands return the expected errors. */
	EXPECT_EQ(variant->expected_fionread_result,
		  test_fionread_ioctl(file_fd));

	/* Checks that unrestrictable commands are unrestricted. */
	EXPECT_EQ(0, ioctl(file_fd, FIOCLEX));
	EXPECT_EQ(0, ioctl(file_fd, FIONCLEX));
	EXPECT_EQ(0, ioctl(file_fd, FIONBIO, &flag));
	EXPECT_EQ(0, ioctl(file_fd, FIOASYNC, &flag));
	EXPECT_EQ(0, ioctl(file_fd, FIGETBSZ, &flag));

	ASSERT_EQ(0, close(file_fd));
}

TEST_F_FORK(ioctl, handle_dir_access_dir)
{
	const int flag = 0;
	const struct rule rules[] = {
		{
			.path = "/dev",
			.access = variant->allowed,
		},
		{},
	};
	int dir_fd, ruleset_fd;

	/* Enables Landlock. */
	ruleset_fd = create_ruleset(_metadata, variant->handled, rules);
	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/*
	 * Ignore variant->open_mode for this test, as we intend to open a
	 * directory.  If the directory can not be opened, the variant is
	 * infeasible to test with an opened directory.
	 */
	dir_fd = open("/dev", O_RDONLY);
	if (dir_fd < 0)
		return;

	/*
	 * Checks that IOCTL commands return the expected errors.
	 * We do not use the expected values from the fixture here.
	 *
	 * When using IOCTL on a directory, no Landlock restrictions apply.
	 */
	EXPECT_EQ(0, test_fionread_ioctl(dir_fd));

	/* Checks that unrestrictable commands are unrestricted. */
	EXPECT_EQ(0, ioctl(dir_fd, FIOCLEX));
	EXPECT_EQ(0, ioctl(dir_fd, FIONCLEX));
	EXPECT_EQ(0, ioctl(dir_fd, FIONBIO, &flag));
	EXPECT_EQ(0, ioctl(dir_fd, FIOASYNC, &flag));
	EXPECT_EQ(0, ioctl(dir_fd, FIGETBSZ, &flag));

	ASSERT_EQ(0, close(dir_fd));
}

TEST_F_FORK(ioctl, handle_file_access_file)
{
	const int flag = 0;
	const struct rule rules[] = {
		{
			.path = "/dev/zero",
			.access = variant->allowed,
		},
		{},
	};
	int file_fd, ruleset_fd;

	/* Enables Landlock. */
	ruleset_fd = create_ruleset(_metadata, variant->handled, rules);
	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	file_fd = open("/dev/zero", variant->open_mode);
	ASSERT_LE(0, file_fd)
	{
		TH_LOG("Failed to open /dev/zero: %s", strerror(errno));
	}

	/* Checks that IOCTL commands return the expected errors. */
	EXPECT_EQ(variant->expected_fionread_result,
		  test_fionread_ioctl(file_fd));

	/* Checks that unrestrictable commands are unrestricted. */
	EXPECT_EQ(0, ioctl(file_fd, FIOCLEX));
	EXPECT_EQ(0, ioctl(file_fd, FIONCLEX));
	EXPECT_EQ(0, ioctl(file_fd, FIONBIO, &flag));
	EXPECT_EQ(0, ioctl(file_fd, FIOASYNC, &flag));
	EXPECT_EQ(0, ioctl(file_fd, FIGETBSZ, &flag));

	ASSERT_EQ(0, close(file_fd));
}

/* clang-format off */
FIXTURE(layout1_bind) {};
/* clang-format on */

FIXTURE_SETUP(layout1_bind)
{
	prepare_layout(_metadata);

	create_layout1(_metadata);

	set_cap(_metadata, CAP_SYS_ADMIN);
	ASSERT_EQ(0, mount(dir_s1d2, dir_s2d2, NULL, MS_BIND, NULL));
	clear_cap(_metadata, CAP_SYS_ADMIN);
}

FIXTURE_TEARDOWN_PARENT(layout1_bind)
{
	/* umount(dir_s2d2)) is handled by namespace lifetime. */

	remove_layout1(_metadata);

	cleanup_layout(_metadata);
}

static const char bind_dir_s1d3[] = TMP_DIR "/s2d1/s2d2/s1d3";
static const char bind_file1_s1d3[] = TMP_DIR "/s2d1/s2d2/s1d3/f1";

/*
 * layout1_bind hierarchy:
 *
 * tmp
 * ├── s1d1
 * │   ├── f1
 * │   ├── f2
 * │   └── s1d2
 * │       ├── f1
 * │       ├── f2
 * │       └── s1d3
 * │           ├── f1
 * │           └── f2
 * ├── s2d1
 * │   ├── f1
 * │   └── s2d2
 * │       ├── f1
 * │       ├── f2
 * │       └── s1d3
 * │           ├── f1
 * │           └── f2
 * └── s3d1
 *     └── s3d2
 *         └── s3d3
 */

TEST_F_FORK(layout1_bind, no_restriction)
{
	ASSERT_EQ(0, test_open(dir_s1d1, O_RDONLY));
	ASSERT_EQ(0, test_open(file1_s1d1, O_RDONLY));
	ASSERT_EQ(0, test_open(dir_s1d2, O_RDONLY));
	ASSERT_EQ(0, test_open(file1_s1d2, O_RDONLY));
	ASSERT_EQ(0, test_open(dir_s1d3, O_RDONLY));
	ASSERT_EQ(0, test_open(file1_s1d3, O_RDONLY));

	ASSERT_EQ(0, test_open(dir_s2d1, O_RDONLY));
	ASSERT_EQ(0, test_open(file1_s2d1, O_RDONLY));
	ASSERT_EQ(0, test_open(dir_s2d2, O_RDONLY));
	ASSERT_EQ(0, test_open(file1_s2d2, O_RDONLY));
	ASSERT_EQ(ENOENT, test_open(dir_s2d3, O_RDONLY));
	ASSERT_EQ(ENOENT, test_open(file1_s2d3, O_RDONLY));

	ASSERT_EQ(0, test_open(bind_dir_s1d3, O_RDONLY));
	ASSERT_EQ(0, test_open(bind_file1_s1d3, O_RDONLY));

	ASSERT_EQ(0, test_open(dir_s3d1, O_RDONLY));
}

TEST_F_FORK(layout1_bind, same_content_same_file)
{
	/*
	 * Sets access right on parent directories of both source and
	 * destination mount points.
	 */
	const struct rule layer1_parent[] = {
		{
			.path = dir_s1d1,
			.access = ACCESS_RO,
		},
		{
			.path = dir_s2d1,
			.access = ACCESS_RW,
		},
		{},
	};
	/*
	 * Sets access rights on the same bind-mounted directories.  The result
	 * should be ACCESS_RW for both directories, but not both hierarchies
	 * because of the first layer.
	 */
	const struct rule layer2_mount_point[] = {
		{
			.path = dir_s1d2,
			.access = LANDLOCK_ACCESS_FS_READ_FILE,
		},
		{
			.path = dir_s2d2,
			.access = ACCESS_RW,
		},
		{},
	};
	/* Only allow read-access to the s1d3 hierarchies. */
	const struct rule layer3_source[] = {
		{
			.path = dir_s1d3,
			.access = LANDLOCK_ACCESS_FS_READ_FILE,
		},
		{},
	};
	/* Removes all access rights. */
	const struct rule layer4_destination[] = {
		{
			.path = bind_file1_s1d3,
			.access = LANDLOCK_ACCESS_FS_WRITE_FILE,
		},
		{},
	};
	int ruleset_fd;

	/* Sets rules for the parent directories. */
	ruleset_fd = create_ruleset(_metadata, ACCESS_RW, layer1_parent);
	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/* Checks source hierarchy. */
	ASSERT_EQ(0, test_open(file1_s1d1, O_RDONLY));
	ASSERT_EQ(EACCES, test_open(file1_s1d1, O_WRONLY));
	ASSERT_EQ(0, test_open(dir_s1d1, O_RDONLY | O_DIRECTORY));

	ASSERT_EQ(0, test_open(file1_s1d2, O_RDONLY));
	ASSERT_EQ(EACCES, test_open(file1_s1d2, O_WRONLY));
	ASSERT_EQ(0, test_open(dir_s1d2, O_RDONLY | O_DIRECTORY));

	/* Checks destination hierarchy. */
	ASSERT_EQ(0, test_open(file1_s2d1, O_RDWR));
	ASSERT_EQ(0, test_open(dir_s2d1, O_RDONLY | O_DIRECTORY));

	ASSERT_EQ(0, test_open(file1_s2d2, O_RDWR));
	ASSERT_EQ(0, test_open(dir_s2d2, O_RDONLY | O_DIRECTORY));

	/* Sets rules for the mount points. */
	ruleset_fd = create_ruleset(_metadata, ACCESS_RW, layer2_mount_point);
	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/* Checks source hierarchy. */
	ASSERT_EQ(EACCES, test_open(file1_s1d1, O_RDONLY));
	ASSERT_EQ(EACCES, test_open(file1_s1d1, O_WRONLY));
	ASSERT_EQ(EACCES, test_open(dir_s1d1, O_RDONLY | O_DIRECTORY));

	ASSERT_EQ(0, test_open(file1_s1d2, O_RDONLY));
	ASSERT_EQ(EACCES, test_open(file1_s1d2, O_WRONLY));
	ASSERT_EQ(0, test_open(dir_s1d2, O_RDONLY | O_DIRECTORY));

	/* Checks destination hierarchy. */
	ASSERT_EQ(EACCES, test_open(file1_s2d1, O_RDONLY));
	ASSERT_EQ(EACCES, test_open(file1_s2d1, O_WRONLY));
	ASSERT_EQ(EACCES, test_open(dir_s2d1, O_RDONLY | O_DIRECTORY));

	ASSERT_EQ(0, test_open(file1_s2d2, O_RDWR));
	ASSERT_EQ(0, test_open(dir_s2d2, O_RDONLY | O_DIRECTORY));
	ASSERT_EQ(0, test_open(bind_dir_s1d3, O_RDONLY | O_DIRECTORY));

	/* Sets a (shared) rule only on the source. */
	ruleset_fd = create_ruleset(_metadata, ACCESS_RW, layer3_source);
	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/* Checks source hierarchy. */
	ASSERT_EQ(EACCES, test_open(file1_s1d2, O_RDONLY));
	ASSERT_EQ(EACCES, test_open(file1_s1d2, O_WRONLY));
	ASSERT_EQ(EACCES, test_open(dir_s1d2, O_RDONLY | O_DIRECTORY));

	ASSERT_EQ(0, test_open(file1_s1d3, O_RDONLY));
	ASSERT_EQ(EACCES, test_open(file1_s1d3, O_WRONLY));
	ASSERT_EQ(EACCES, test_open(dir_s1d3, O_RDONLY | O_DIRECTORY));

	/* Checks destination hierarchy. */
	ASSERT_EQ(EACCES, test_open(file1_s2d2, O_RDONLY));
	ASSERT_EQ(EACCES, test_open(file1_s2d2, O_WRONLY));
	ASSERT_EQ(EACCES, test_open(dir_s2d2, O_RDONLY | O_DIRECTORY));

	ASSERT_EQ(0, test_open(bind_file1_s1d3, O_RDONLY));
	ASSERT_EQ(EACCES, test_open(bind_file1_s1d3, O_WRONLY));
	ASSERT_EQ(EACCES, test_open(bind_dir_s1d3, O_RDONLY | O_DIRECTORY));

	/* Sets a (shared) rule only on the destination. */
	ruleset_fd = create_ruleset(_metadata, ACCESS_RW, layer4_destination);
	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/* Checks source hierarchy. */
	ASSERT_EQ(EACCES, test_open(file1_s1d3, O_RDONLY));
	ASSERT_EQ(EACCES, test_open(file1_s1d3, O_WRONLY));

	/* Checks destination hierarchy. */
	ASSERT_EQ(EACCES, test_open(bind_file1_s1d3, O_RDONLY));
	ASSERT_EQ(EACCES, test_open(bind_file1_s1d3, O_WRONLY));
}

TEST_F_FORK(layout1_bind, reparent_cross_mount)
{
	const struct rule layer1[] = {
		{
			/* dir_s2d1 is beneath the dir_s2d2 mount point. */
			.path = dir_s2d1,
			.access = LANDLOCK_ACCESS_FS_REFER,
		},
		{
			.path = bind_dir_s1d3,
			.access = LANDLOCK_ACCESS_FS_EXECUTE,
		},
		{},
	};
	int ruleset_fd = create_ruleset(
		_metadata,
		LANDLOCK_ACCESS_FS_REFER | LANDLOCK_ACCESS_FS_EXECUTE, layer1);

	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/* Checks basic denied move. */
	ASSERT_EQ(-1, rename(file1_s1d1, file1_s1d2));
	ASSERT_EQ(EXDEV, errno);

	/* Checks real cross-mount move (Landlock is not involved). */
	ASSERT_EQ(-1, rename(file1_s2d1, file1_s2d2));
	ASSERT_EQ(EXDEV, errno);

	/* Checks move that will give more accesses. */
	ASSERT_EQ(-1, rename(file1_s2d2, bind_file1_s1d3));
	ASSERT_EQ(EXDEV, errno);

	/* Checks legitimate downgrade move. */
	ASSERT_EQ(0, rename(bind_file1_s1d3, file1_s2d2));
}

#define LOWER_BASE TMP_DIR "/lower"
#define LOWER_DATA LOWER_BASE "/data"
static const char lower_fl1[] = LOWER_DATA "/fl1";
static const char lower_dl1[] = LOWER_DATA "/dl1";
static const char lower_dl1_fl2[] = LOWER_DATA "/dl1/fl2";
static const char lower_fo1[] = LOWER_DATA "/fo1";
static const char lower_do1[] = LOWER_DATA "/do1";
static const char lower_do1_fo2[] = LOWER_DATA "/do1/fo2";
static const char lower_do1_fl3[] = LOWER_DATA "/do1/fl3";

static const char (*lower_base_files[])[] = {
	&lower_fl1,
	&lower_fo1,
	NULL,
};
static const char (*lower_base_directories[])[] = {
	&lower_dl1,
	&lower_do1,
	NULL,
};
static const char (*lower_sub_files[])[] = {
	&lower_dl1_fl2,
	&lower_do1_fo2,
	&lower_do1_fl3,
	NULL,
};

#define UPPER_BASE TMP_DIR "/upper"
#define UPPER_DATA UPPER_BASE "/data"
#define UPPER_WORK UPPER_BASE "/work"
static const char upper_fu1[] = UPPER_DATA "/fu1";
static const char upper_du1[] = UPPER_DATA "/du1";
static const char upper_du1_fu2[] = UPPER_DATA "/du1/fu2";
static const char upper_fo1[] = UPPER_DATA "/fo1";
static const char upper_do1[] = UPPER_DATA "/do1";
static const char upper_do1_fo2[] = UPPER_DATA "/do1/fo2";
static const char upper_do1_fu3[] = UPPER_DATA "/do1/fu3";

static const char (*upper_base_files[])[] = {
	&upper_fu1,
	&upper_fo1,
	NULL,
};
static const char (*upper_base_directories[])[] = {
	&upper_du1,
	&upper_do1,
	NULL,
};
static const char (*upper_sub_files[])[] = {
	&upper_du1_fu2,
	&upper_do1_fo2,
	&upper_do1_fu3,
	NULL,
};

#define MERGE_BASE TMP_DIR "/merge"
#define MERGE_DATA MERGE_BASE "/data"
static const char merge_fl1[] = MERGE_DATA "/fl1";
static const char merge_dl1[] = MERGE_DATA "/dl1";
static const char merge_dl1_fl2[] = MERGE_DATA "/dl1/fl2";
static const char merge_fu1[] = MERGE_DATA "/fu1";
static const char merge_du1[] = MERGE_DATA "/du1";
static const char merge_du1_fu2[] = MERGE_DATA "/du1/fu2";
static const char merge_fo1[] = MERGE_DATA "/fo1";
static const char merge_do1[] = MERGE_DATA "/do1";
static const char merge_do1_fo2[] = MERGE_DATA "/do1/fo2";
static const char merge_do1_fl3[] = MERGE_DATA "/do1/fl3";
static const char merge_do1_fu3[] = MERGE_DATA "/do1/fu3";

static const char (*merge_base_files[])[] = {
	&merge_fl1,
	&merge_fu1,
	&merge_fo1,
	NULL,
};
static const char (*merge_base_directories[])[] = {
	&merge_dl1,
	&merge_du1,
	&merge_do1,
	NULL,
};
static const char (*merge_sub_files[])[] = {
	&merge_dl1_fl2, &merge_du1_fu2, &merge_do1_fo2,
	&merge_do1_fl3, &merge_do1_fu3, NULL,
};

/*
 * layout2_overlay hierarchy:
 *
 * tmp
 * ├── lower
 * │   └── data
 * │       ├── dl1
 * │       │   └── fl2
 * │       ├── do1
 * │       │   ├── fl3
 * │       │   └── fo2
 * │       ├── fl1
 * │       └── fo1
 * ├── merge
 * │   └── data
 * │       ├── dl1
 * │       │   └── fl2
 * │       ├── do1
 * │       │   ├── fl3
 * │       │   ├── fo2
 * │       │   └── fu3
 * │       ├── du1
 * │       │   └── fu2
 * │       ├── fl1
 * │       ├── fo1
 * │       └── fu1
 * └── upper
 *     ├── data
 *     │   ├── do1
 *     │   │   ├── fo2
 *     │   │   └── fu3
 *     │   ├── du1
 *     │   │   └── fu2
 *     │   ├── fo1
 *     │   └── fu1
 *     └── work
 *         └── work
 */

FIXTURE(layout2_overlay)
{
	bool skip_test;
};

FIXTURE_SETUP(layout2_overlay)
{
	if (!supports_filesystem("overlay")) {
		self->skip_test = true;
		SKIP(return, "overlayfs is not supported (setup)");
	}

	prepare_layout(_metadata);

	create_directory(_metadata, LOWER_BASE);
	set_cap(_metadata, CAP_SYS_ADMIN);
	/* Creates tmpfs mount points to get deterministic overlayfs. */
	ASSERT_EQ(0, mount_opt(&mnt_tmp, LOWER_BASE));
	clear_cap(_metadata, CAP_SYS_ADMIN);
	create_file(_metadata, lower_fl1);
	create_file(_metadata, lower_dl1_fl2);
	create_file(_metadata, lower_fo1);
	create_file(_metadata, lower_do1_fo2);
	create_file(_metadata, lower_do1_fl3);

	create_directory(_metadata, UPPER_BASE);
	set_cap(_metadata, CAP_SYS_ADMIN);
	ASSERT_EQ(0, mount_opt(&mnt_tmp, UPPER_BASE));
	clear_cap(_metadata, CAP_SYS_ADMIN);
	create_file(_metadata, upper_fu1);
	create_file(_metadata, upper_du1_fu2);
	create_file(_metadata, upper_fo1);
	create_file(_metadata, upper_do1_fo2);
	create_file(_metadata, upper_do1_fu3);
	ASSERT_EQ(0, mkdir(UPPER_WORK, 0700));

	create_directory(_metadata, MERGE_DATA);
	set_cap(_metadata, CAP_SYS_ADMIN);
	set_cap(_metadata, CAP_DAC_OVERRIDE);
	ASSERT_EQ(0, mount("overlay", MERGE_DATA, "overlay", 0,
			   "lowerdir=" LOWER_DATA ",upperdir=" UPPER_DATA
			   ",workdir=" UPPER_WORK));
	clear_cap(_metadata, CAP_DAC_OVERRIDE);
	clear_cap(_metadata, CAP_SYS_ADMIN);
}

FIXTURE_TEARDOWN_PARENT(layout2_overlay)
{
	if (self->skip_test)
		SKIP(return, "overlayfs is not supported (teardown)");

	EXPECT_EQ(0, remove_path(lower_do1_fl3));
	EXPECT_EQ(0, remove_path(lower_dl1_fl2));
	EXPECT_EQ(0, remove_path(lower_fl1));
	EXPECT_EQ(0, remove_path(lower_do1_fo2));
	EXPECT_EQ(0, remove_path(lower_fo1));

	/* umount(LOWER_BASE)) is handled by namespace lifetime. */
	EXPECT_EQ(0, remove_path(LOWER_BASE));

	EXPECT_EQ(0, remove_path(upper_do1_fu3));
	EXPECT_EQ(0, remove_path(upper_du1_fu2));
	EXPECT_EQ(0, remove_path(upper_fu1));
	EXPECT_EQ(0, remove_path(upper_do1_fo2));
	EXPECT_EQ(0, remove_path(upper_fo1));
	EXPECT_EQ(0, remove_path(UPPER_WORK "/work"));

	/* umount(UPPER_BASE)) is handled by namespace lifetime. */
	EXPECT_EQ(0, remove_path(UPPER_BASE));

	/* umount(MERGE_DATA)) is handled by namespace lifetime. */
	EXPECT_EQ(0, remove_path(MERGE_DATA));

	cleanup_layout(_metadata);
}

TEST_F_FORK(layout2_overlay, no_restriction)
{
	if (self->skip_test)
		SKIP(return, "overlayfs is not supported (test)");

	ASSERT_EQ(0, test_open(lower_fl1, O_RDONLY));
	ASSERT_EQ(0, test_open(lower_dl1, O_RDONLY));
	ASSERT_EQ(0, test_open(lower_dl1_fl2, O_RDONLY));
	ASSERT_EQ(0, test_open(lower_fo1, O_RDONLY));
	ASSERT_EQ(0, test_open(lower_do1, O_RDONLY));
	ASSERT_EQ(0, test_open(lower_do1_fo2, O_RDONLY));
	ASSERT_EQ(0, test_open(lower_do1_fl3, O_RDONLY));

	ASSERT_EQ(0, test_open(upper_fu1, O_RDONLY));
	ASSERT_EQ(0, test_open(upper_du1, O_RDONLY));
	ASSERT_EQ(0, test_open(upper_du1_fu2, O_RDONLY));
	ASSERT_EQ(0, test_open(upper_fo1, O_RDONLY));
	ASSERT_EQ(0, test_open(upper_do1, O_RDONLY));
	ASSERT_EQ(0, test_open(upper_do1_fo2, O_RDONLY));
	ASSERT_EQ(0, test_open(upper_do1_fu3, O_RDONLY));

	ASSERT_EQ(0, test_open(merge_fl1, O_RDONLY));
	ASSERT_EQ(0, test_open(merge_dl1, O_RDONLY));
	ASSERT_EQ(0, test_open(merge_dl1_fl2, O_RDONLY));
	ASSERT_EQ(0, test_open(merge_fu1, O_RDONLY));
	ASSERT_EQ(0, test_open(merge_du1, O_RDONLY));
	ASSERT_EQ(0, test_open(merge_du1_fu2, O_RDONLY));
	ASSERT_EQ(0, test_open(merge_fo1, O_RDONLY));
	ASSERT_EQ(0, test_open(merge_do1, O_RDONLY));
	ASSERT_EQ(0, test_open(merge_do1_fo2, O_RDONLY));
	ASSERT_EQ(0, test_open(merge_do1_fl3, O_RDONLY));
	ASSERT_EQ(0, test_open(merge_do1_fu3, O_RDONLY));
}

#define for_each_path(path_list, path_entry, i)               \
	for (i = 0, path_entry = *path_list[i]; path_list[i]; \
	     path_entry = *path_list[++i])

TEST_F_FORK(layout2_overlay, same_content_different_file)
{
	/* Sets access right on parent directories of both layers. */
	const struct rule layer1_base[] = {
		{
			.path = LOWER_BASE,
			.access = LANDLOCK_ACCESS_FS_READ_FILE,
		},
		{
			.path = UPPER_BASE,
			.access = LANDLOCK_ACCESS_FS_READ_FILE,
		},
		{
			.path = MERGE_BASE,
			.access = ACCESS_RW,
		},
		{},
	};
	const struct rule layer2_data[] = {
		{
			.path = LOWER_DATA,
			.access = LANDLOCK_ACCESS_FS_READ_FILE,
		},
		{
			.path = UPPER_DATA,
			.access = LANDLOCK_ACCESS_FS_READ_FILE,
		},
		{
			.path = MERGE_DATA,
			.access = ACCESS_RW,
		},
		{},
	};
	/* Sets access right on directories inside both layers. */
	const struct rule layer3_subdirs[] = {
		{
			.path = lower_dl1,
			.access = LANDLOCK_ACCESS_FS_READ_FILE,
		},
		{
			.path = lower_do1,
			.access = LANDLOCK_ACCESS_FS_READ_FILE,
		},
		{
			.path = upper_du1,
			.access = LANDLOCK_ACCESS_FS_READ_FILE,
		},
		{
			.path = upper_do1,
			.access = LANDLOCK_ACCESS_FS_READ_FILE,
		},
		{
			.path = merge_dl1,
			.access = ACCESS_RW,
		},
		{
			.path = merge_du1,
			.access = ACCESS_RW,
		},
		{
			.path = merge_do1,
			.access = ACCESS_RW,
		},
		{},
	};
	/* Tighten access rights to the files. */
	const struct rule layer4_files[] = {
		{
			.path = lower_dl1_fl2,
			.access = LANDLOCK_ACCESS_FS_READ_FILE,
		},
		{
			.path = lower_do1_fo2,
			.access = LANDLOCK_ACCESS_FS_READ_FILE,
		},
		{
			.path = lower_do1_fl3,
			.access = LANDLOCK_ACCESS_FS_READ_FILE,
		},
		{
			.path = upper_du1_fu2,
			.access = LANDLOCK_ACCESS_FS_READ_FILE,
		},
		{
			.path = upper_do1_fo2,
			.access = LANDLOCK_ACCESS_FS_READ_FILE,
		},
		{
			.path = upper_do1_fu3,
			.access = LANDLOCK_ACCESS_FS_READ_FILE,
		},
		{
			.path = merge_dl1_fl2,
			.access = LANDLOCK_ACCESS_FS_READ_FILE |
				  LANDLOCK_ACCESS_FS_WRITE_FILE,
		},
		{
			.path = merge_du1_fu2,
			.access = LANDLOCK_ACCESS_FS_READ_FILE |
				  LANDLOCK_ACCESS_FS_WRITE_FILE,
		},
		{
			.path = merge_do1_fo2,
			.access = LANDLOCK_ACCESS_FS_READ_FILE |
				  LANDLOCK_ACCESS_FS_WRITE_FILE,
		},
		{
			.path = merge_do1_fl3,
			.access = LANDLOCK_ACCESS_FS_READ_FILE |
				  LANDLOCK_ACCESS_FS_WRITE_FILE,
		},
		{
			.path = merge_do1_fu3,
			.access = LANDLOCK_ACCESS_FS_READ_FILE |
				  LANDLOCK_ACCESS_FS_WRITE_FILE,
		},
		{},
	};
	const struct rule layer5_merge_only[] = {
		{
			.path = MERGE_DATA,
			.access = LANDLOCK_ACCESS_FS_READ_FILE |
				  LANDLOCK_ACCESS_FS_WRITE_FILE,
		},
		{},
	};
	int ruleset_fd;
	size_t i;
	const char *path_entry;

	if (self->skip_test)
		SKIP(return, "overlayfs is not supported (test)");

	/* Sets rules on base directories (i.e. outside overlay scope). */
	ruleset_fd = create_ruleset(_metadata, ACCESS_RW, layer1_base);
	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/* Checks lower layer. */
	for_each_path(lower_base_files, path_entry, i) {
		ASSERT_EQ(0, test_open(path_entry, O_RDONLY));
		ASSERT_EQ(EACCES, test_open(path_entry, O_WRONLY));
	}
	for_each_path(lower_base_directories, path_entry, i) {
		ASSERT_EQ(EACCES,
			  test_open(path_entry, O_RDONLY | O_DIRECTORY));
	}
	for_each_path(lower_sub_files, path_entry, i) {
		ASSERT_EQ(0, test_open(path_entry, O_RDONLY));
		ASSERT_EQ(EACCES, test_open(path_entry, O_WRONLY));
	}
	/* Checks upper layer. */
	for_each_path(upper_base_files, path_entry, i) {
		ASSERT_EQ(0, test_open(path_entry, O_RDONLY));
		ASSERT_EQ(EACCES, test_open(path_entry, O_WRONLY));
	}
	for_each_path(upper_base_directories, path_entry, i) {
		ASSERT_EQ(EACCES,
			  test_open(path_entry, O_RDONLY | O_DIRECTORY));
	}
	for_each_path(upper_sub_files, path_entry, i) {
		ASSERT_EQ(0, test_open(path_entry, O_RDONLY));
		ASSERT_EQ(EACCES, test_open(path_entry, O_WRONLY));
	}
	/*
	 * Checks that access rights are independent from the lower and upper
	 * layers: write access to upper files viewed through the merge point
	 * is still allowed, and write access to lower file viewed (and copied)
	 * through the merge point is still allowed.
	 */
	for_each_path(merge_base_files, path_entry, i) {
		ASSERT_EQ(0, test_open(path_entry, O_RDWR));
	}
	for_each_path(merge_base_directories, path_entry, i) {
		ASSERT_EQ(0, test_open(path_entry, O_RDONLY | O_DIRECTORY));
	}
	for_each_path(merge_sub_files, path_entry, i) {
		ASSERT_EQ(0, test_open(path_entry, O_RDWR));
	}

	/* Sets rules on data directories (i.e. inside overlay scope). */
	ruleset_fd = create_ruleset(_metadata, ACCESS_RW, layer2_data);
	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/* Checks merge. */
	for_each_path(merge_base_files, path_entry, i) {
		ASSERT_EQ(0, test_open(path_entry, O_RDWR));
	}
	for_each_path(merge_base_directories, path_entry, i) {
		ASSERT_EQ(0, test_open(path_entry, O_RDONLY | O_DIRECTORY));
	}
	for_each_path(merge_sub_files, path_entry, i) {
		ASSERT_EQ(0, test_open(path_entry, O_RDWR));
	}

	/* Same checks with tighter rules. */
	ruleset_fd = create_ruleset(_metadata, ACCESS_RW, layer3_subdirs);
	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/* Checks changes for lower layer. */
	for_each_path(lower_base_files, path_entry, i) {
		ASSERT_EQ(EACCES, test_open(path_entry, O_RDONLY));
	}
	/* Checks changes for upper layer. */
	for_each_path(upper_base_files, path_entry, i) {
		ASSERT_EQ(EACCES, test_open(path_entry, O_RDONLY));
	}
	/* Checks all merge accesses. */
	for_each_path(merge_base_files, path_entry, i) {
		ASSERT_EQ(EACCES, test_open(path_entry, O_RDWR));
	}
	for_each_path(merge_base_directories, path_entry, i) {
		ASSERT_EQ(0, test_open(path_entry, O_RDONLY | O_DIRECTORY));
	}
	for_each_path(merge_sub_files, path_entry, i) {
		ASSERT_EQ(0, test_open(path_entry, O_RDWR));
	}

	/* Sets rules directly on overlayed files. */
	ruleset_fd = create_ruleset(_metadata, ACCESS_RW, layer4_files);
	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/* Checks unchanged accesses on lower layer. */
	for_each_path(lower_sub_files, path_entry, i) {
		ASSERT_EQ(0, test_open(path_entry, O_RDONLY));
		ASSERT_EQ(EACCES, test_open(path_entry, O_WRONLY));
	}
	/* Checks unchanged accesses on upper layer. */
	for_each_path(upper_sub_files, path_entry, i) {
		ASSERT_EQ(0, test_open(path_entry, O_RDONLY));
		ASSERT_EQ(EACCES, test_open(path_entry, O_WRONLY));
	}
	/* Checks all merge accesses. */
	for_each_path(merge_base_files, path_entry, i) {
		ASSERT_EQ(EACCES, test_open(path_entry, O_RDWR));
	}
	for_each_path(merge_base_directories, path_entry, i) {
		ASSERT_EQ(EACCES,
			  test_open(path_entry, O_RDONLY | O_DIRECTORY));
	}
	for_each_path(merge_sub_files, path_entry, i) {
		ASSERT_EQ(0, test_open(path_entry, O_RDWR));
	}

	/* Only allowes access to the merge hierarchy. */
	ruleset_fd = create_ruleset(_metadata, ACCESS_RW, layer5_merge_only);
	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/* Checks new accesses on lower layer. */
	for_each_path(lower_sub_files, path_entry, i) {
		ASSERT_EQ(EACCES, test_open(path_entry, O_RDONLY));
	}
	/* Checks new accesses on upper layer. */
	for_each_path(upper_sub_files, path_entry, i) {
		ASSERT_EQ(EACCES, test_open(path_entry, O_RDONLY));
	}
	/* Checks all merge accesses. */
	for_each_path(merge_base_files, path_entry, i) {
		ASSERT_EQ(EACCES, test_open(path_entry, O_RDWR));
	}
	for_each_path(merge_base_directories, path_entry, i) {
		ASSERT_EQ(EACCES,
			  test_open(path_entry, O_RDONLY | O_DIRECTORY));
	}
	for_each_path(merge_sub_files, path_entry, i) {
		ASSERT_EQ(0, test_open(path_entry, O_RDWR));
	}
}

FIXTURE(layout3_fs)
{
	bool has_created_dir;
	bool has_created_file;
	bool skip_test;
};

FIXTURE_VARIANT(layout3_fs)
{
	const struct mnt_opt mnt;
	const char *const file_path;
	unsigned int cwd_fs_magic;
};

/* clang-format off */
FIXTURE_VARIANT_ADD(layout3_fs, tmpfs) {
	/* clang-format on */
	.mnt = {
		.type = "tmpfs",
		.data = MNT_TMP_DATA,
	},
	.file_path = file1_s1d1,
};

FIXTURE_VARIANT_ADD(layout3_fs, ramfs) {
	.mnt = {
		.type = "ramfs",
		.data = "mode=700",
	},
	.file_path = TMP_DIR "/dir/file",
};

FIXTURE_VARIANT_ADD(layout3_fs, cgroup2) {
	.mnt = {
		.type = "cgroup2",
	},
	.file_path = TMP_DIR "/test/cgroup.procs",
};

FIXTURE_VARIANT_ADD(layout3_fs, proc) {
	.mnt = {
		.type = "proc",
	},
	.file_path = TMP_DIR "/self/status",
};

FIXTURE_VARIANT_ADD(layout3_fs, sysfs) {
	.mnt = {
		.type = "sysfs",
	},
	.file_path = TMP_DIR "/kernel/notes",
};

FIXTURE_VARIANT_ADD(layout3_fs, hostfs) {
	.mnt = {
		.source = TMP_DIR,
		.flags = MS_BIND,
	},
	.file_path = TMP_DIR "/dir/file",
	.cwd_fs_magic = HOSTFS_SUPER_MAGIC,
};

static char *dirname_alloc(const char *path)
{
	char *dup;

	if (!path)
		return NULL;

	dup = strdup(path);
	if (!dup)
		return NULL;

	return dirname(dup);
}

FIXTURE_SETUP(layout3_fs)
{
	struct stat statbuf;
	char *dir_path = dirname_alloc(variant->file_path);

	if (!supports_filesystem(variant->mnt.type) ||
	    !cwd_matches_fs(variant->cwd_fs_magic)) {
		self->skip_test = true;
		SKIP(return, "this filesystem is not supported (setup)");
	}

	prepare_layout_opt(_metadata, &variant->mnt);

	/* Creates directory when required. */
	if (stat(dir_path, &statbuf)) {
		set_cap(_metadata, CAP_DAC_OVERRIDE);
		EXPECT_EQ(0, mkdir(dir_path, 0700))
		{
			TH_LOG("Failed to create directory \"%s\": %s",
			       dir_path, strerror(errno));
		}
		self->has_created_dir = true;
		clear_cap(_metadata, CAP_DAC_OVERRIDE);
	}

	/* Creates file when required. */
	if (stat(variant->file_path, &statbuf)) {
		int fd;

		set_cap(_metadata, CAP_DAC_OVERRIDE);
		fd = creat(variant->file_path, 0600);
		EXPECT_LE(0, fd)
		{
			TH_LOG("Failed to create file \"%s\": %s",
			       variant->file_path, strerror(errno));
		}
		EXPECT_EQ(0, close(fd));
		self->has_created_file = true;
		clear_cap(_metadata, CAP_DAC_OVERRIDE);
	}

	free(dir_path);
}

FIXTURE_TEARDOWN_PARENT(layout3_fs)
{
	if (self->skip_test)
		SKIP(return, "this filesystem is not supported (teardown)");

	if (self->has_created_file) {
		set_cap(_metadata, CAP_DAC_OVERRIDE);
		/*
		 * Don't check for error because the file might already
		 * have been removed (cf. release_inode test).
		 */
		unlink(variant->file_path);
		clear_cap(_metadata, CAP_DAC_OVERRIDE);
	}

	if (self->has_created_dir) {
		char *dir_path = dirname_alloc(variant->file_path);

		set_cap(_metadata, CAP_DAC_OVERRIDE);
		/*
		 * Don't check for error because the directory might already
		 * have been removed (cf. release_inode test).
		 */
		rmdir(dir_path);
		clear_cap(_metadata, CAP_DAC_OVERRIDE);
		free(dir_path);
	}

	cleanup_layout(_metadata);
}

static void layer3_fs_tag_inode(struct __test_metadata *const _metadata,
				FIXTURE_DATA(layout3_fs) * self,
				const FIXTURE_VARIANT(layout3_fs) * variant,
				const char *const rule_path)
{
	const struct rule layer1_allow_read_file[] = {
		{
			.path = rule_path,
			.access = LANDLOCK_ACCESS_FS_READ_FILE,
		},
		{},
	};
	const struct landlock_ruleset_attr layer2_deny_everything_attr = {
		.handled_access_fs = LANDLOCK_ACCESS_FS_READ_FILE,
	};
	const char *const dev_null_path = "/dev/null";
	int ruleset_fd;

	if (self->skip_test)
		SKIP(return, "this filesystem is not supported (test)");

	/* Checks without Landlock. */
	EXPECT_EQ(0, test_open(dev_null_path, O_RDONLY | O_CLOEXEC));
	EXPECT_EQ(0, test_open(variant->file_path, O_RDONLY | O_CLOEXEC));

	ruleset_fd = create_ruleset(_metadata, LANDLOCK_ACCESS_FS_READ_FILE,
				    layer1_allow_read_file);
	EXPECT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	EXPECT_EQ(0, close(ruleset_fd));

	EXPECT_EQ(EACCES, test_open(dev_null_path, O_RDONLY | O_CLOEXEC));
	EXPECT_EQ(0, test_open(variant->file_path, O_RDONLY | O_CLOEXEC));

	/* Forbids directory reading. */
	ruleset_fd =
		landlock_create_ruleset(&layer2_deny_everything_attr,
					sizeof(layer2_deny_everything_attr), 0);
	EXPECT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	EXPECT_EQ(0, close(ruleset_fd));

	/* Checks with Landlock and forbidden access. */
	EXPECT_EQ(EACCES, test_open(dev_null_path, O_RDONLY | O_CLOEXEC));
	EXPECT_EQ(EACCES, test_open(variant->file_path, O_RDONLY | O_CLOEXEC));
}

/* Matrix of tests to check file hierarchy evaluation. */

TEST_F_FORK(layout3_fs, tag_inode_dir_parent)
{
	/* The current directory must not be the root for this test. */
	layer3_fs_tag_inode(_metadata, self, variant, ".");
}

TEST_F_FORK(layout3_fs, tag_inode_dir_mnt)
{
	layer3_fs_tag_inode(_metadata, self, variant, TMP_DIR);
}

TEST_F_FORK(layout3_fs, tag_inode_dir_child)
{
	char *dir_path = dirname_alloc(variant->file_path);

	layer3_fs_tag_inode(_metadata, self, variant, dir_path);
	free(dir_path);
}

TEST_F_FORK(layout3_fs, tag_inode_file)
{
	layer3_fs_tag_inode(_metadata, self, variant, variant->file_path);
}

/* Light version of layout1.release_inodes */
TEST_F_FORK(layout3_fs, release_inodes)
{
	const struct rule layer1[] = {
		{
			.path = TMP_DIR,
			.access = LANDLOCK_ACCESS_FS_READ_DIR,
		},
		{},
	};
	int ruleset_fd;

	if (self->skip_test)
		SKIP(return, "this filesystem is not supported (test)");

	/* Clean up for the teardown to not fail. */
	if (self->has_created_file)
		EXPECT_EQ(0, remove_path(variant->file_path));

	if (self->has_created_dir) {
		char *dir_path = dirname_alloc(variant->file_path);

		/* Don't check for error because of cgroup specificities. */
		remove_path(dir_path);
		free(dir_path);
	}

	ruleset_fd =
		create_ruleset(_metadata, LANDLOCK_ACCESS_FS_READ_DIR, layer1);
	ASSERT_LE(0, ruleset_fd);

	/* Unmount the filesystem while it is being used by a ruleset. */
	set_cap(_metadata, CAP_SYS_ADMIN);
	ASSERT_EQ(0, umount(TMP_DIR));
	clear_cap(_metadata, CAP_SYS_ADMIN);

	/* Replaces with a new mount point to simplify FIXTURE_TEARDOWN. */
	set_cap(_metadata, CAP_SYS_ADMIN);
	ASSERT_EQ(0, mount_opt(&mnt_tmp, TMP_DIR));
	clear_cap(_metadata, CAP_SYS_ADMIN);

	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	/* Checks that access to the new mount point is denied. */
	ASSERT_EQ(EACCES, test_open(TMP_DIR, O_RDONLY));
}

static int matches_log_fs_extra(struct __test_metadata *const _metadata,
				int audit_fd, const char *const blockers,
				const char *const path, const char *const extra)
{
	static const char log_template[] = REGEX_LANDLOCK_PREFIX
		" blockers=fs\\.%s path=\"%s\" dev=\"[^\"]\\+\" ino=[0-9]\\+$";
	char *absolute_path = NULL;
	size_t log_match_remaining = sizeof(log_template) + strlen(blockers) +
				     PATH_MAX * 2 +
				     (extra ? strlen(extra) : 0) + 1;
	char log_match[log_match_remaining];
	char *log_match_cursor = log_match;
	size_t chunk_len;

	chunk_len = snprintf(log_match_cursor, log_match_remaining,
			     REGEX_LANDLOCK_PREFIX " blockers=%s path=\"",
			     blockers);
	if (chunk_len < 0 || chunk_len >= log_match_remaining)
		return -E2BIG;

	/*
	 * It is assume that absolute_path does not contain control characters nor
	 * spaces, see audit_string_contains_control().
	 */
	absolute_path = realpath(path, NULL);
	if (!absolute_path)
		return -errno;

	log_match_remaining -= chunk_len;
	log_match_cursor += chunk_len;
	log_match_cursor = regex_escape(absolute_path, log_match_cursor,
					log_match_remaining);
	free(absolute_path);
	if (log_match_cursor < 0)
		return (long long)log_match_cursor;

	log_match_remaining -= log_match_cursor - log_match;
	chunk_len = snprintf(log_match_cursor, log_match_remaining,
			     "\" dev=\"[^\"]\\+\" ino=[0-9]\\+%s$",
			     extra ?: "");
	if (chunk_len < 0 || chunk_len >= log_match_remaining)
		return -E2BIG;

	return audit_match_record(audit_fd, AUDIT_LANDLOCK_ACCESS, log_match,
				  NULL);
}

static int matches_log_fs(struct __test_metadata *const _metadata, int audit_fd,
			  const char *const blockers, const char *const path)
{
	return matches_log_fs_extra(_metadata, audit_fd, blockers, path, NULL);
}

FIXTURE(audit_layout1)
{
	struct audit_filter audit_filter;
	int audit_fd;
};

FIXTURE_SETUP(audit_layout1)
{
	prepare_layout(_metadata);

	create_layout1(_metadata);

	set_cap(_metadata, CAP_AUDIT_CONTROL);
	self->audit_fd = audit_init_with_exe_filter(&self->audit_filter);
	EXPECT_LE(0, self->audit_fd);
	disable_caps(_metadata);
}

FIXTURE_TEARDOWN_PARENT(audit_layout1)
{
	remove_layout1(_metadata);

	cleanup_layout(_metadata);

	EXPECT_EQ(0, audit_cleanup(-1, NULL));
}

TEST_F(audit_layout1, execute_make)
{
	struct audit_records records;

	copy_file(_metadata, bin_true, file1_s1d1);
	test_execute(_metadata, 0, file1_s1d1);
	test_check_exec(_metadata, 0, file1_s1d1);

	drop_access_rights(_metadata,
			   &(struct landlock_ruleset_attr){
				   .handled_access_fs =
					   LANDLOCK_ACCESS_FS_EXECUTE,
			   });

	test_execute(_metadata, EACCES, file1_s1d1);
	EXPECT_EQ(0, matches_log_fs(_metadata, self->audit_fd, "fs\\.execute",
				    file1_s1d1));
	test_check_exec(_metadata, EACCES, file1_s1d1);
	EXPECT_EQ(0, matches_log_fs(_metadata, self->audit_fd, "fs\\.execute",
				    file1_s1d1));

	EXPECT_EQ(0, audit_count_records(self->audit_fd, &records));
	EXPECT_EQ(0, records.access);
	EXPECT_EQ(0, records.domain);
}

/*
 * Using a set of handled/denied access rights make it possible to check that
 * only the blocked ones are logged.
 */

/* clang-format off */
static const __u64 access_fs_16 =
	LANDLOCK_ACCESS_FS_EXECUTE |
	LANDLOCK_ACCESS_FS_WRITE_FILE |
	LANDLOCK_ACCESS_FS_READ_FILE |
	LANDLOCK_ACCESS_FS_READ_DIR |
	LANDLOCK_ACCESS_FS_REMOVE_DIR |
	LANDLOCK_ACCESS_FS_REMOVE_FILE |
	LANDLOCK_ACCESS_FS_MAKE_CHAR |
	LANDLOCK_ACCESS_FS_MAKE_DIR |
	LANDLOCK_ACCESS_FS_MAKE_REG |
	LANDLOCK_ACCESS_FS_MAKE_SOCK |
	LANDLOCK_ACCESS_FS_MAKE_FIFO |
	LANDLOCK_ACCESS_FS_MAKE_BLOCK |
	LANDLOCK_ACCESS_FS_MAKE_SYM |
	LANDLOCK_ACCESS_FS_REFER |
	LANDLOCK_ACCESS_FS_TRUNCATE |
	LANDLOCK_ACCESS_FS_IOCTL_DEV;
/* clang-format on */

TEST_F(audit_layout1, execute_read)
{
	struct audit_records records;

	copy_file(_metadata, bin_true, file1_s1d1);
	test_execute(_metadata, 0, file1_s1d1);
	test_check_exec(_metadata, 0, file1_s1d1);

	drop_access_rights(_metadata, &(struct landlock_ruleset_attr){
					      .handled_access_fs = access_fs_16,
				      });

	/*
	 * The only difference with the previous audit_layout1.execute_read test is
	 * the extra ",fs\\.read_file" blocked by the executable file.
	 */
	test_execute(_metadata, EACCES, file1_s1d1);
	EXPECT_EQ(0, matches_log_fs(_metadata, self->audit_fd,
				    "fs\\.execute,fs\\.read_file", file1_s1d1));
	test_check_exec(_metadata, EACCES, file1_s1d1);
	EXPECT_EQ(0, matches_log_fs(_metadata, self->audit_fd,
				    "fs\\.execute,fs\\.read_file", file1_s1d1));

	EXPECT_EQ(0, audit_count_records(self->audit_fd, &records));
	EXPECT_EQ(0, records.access);
	EXPECT_EQ(0, records.domain);
}

TEST_F(audit_layout1, write_file)
{
	struct audit_records records;

	drop_access_rights(_metadata, &(struct landlock_ruleset_attr){
					      .handled_access_fs = access_fs_16,
				      });

	EXPECT_EQ(EACCES, test_open(file1_s1d1, O_WRONLY));
	EXPECT_EQ(0, matches_log_fs(_metadata, self->audit_fd,
				    "fs\\.write_file", file1_s1d1));

	EXPECT_EQ(0, audit_count_records(self->audit_fd, &records));
	EXPECT_EQ(0, records.access);
	EXPECT_EQ(1, records.domain);
}

TEST_F(audit_layout1, read_file)
{
	struct audit_records records;

	drop_access_rights(_metadata, &(struct landlock_ruleset_attr){
					      .handled_access_fs = access_fs_16,
				      });

	EXPECT_EQ(EACCES, test_open(file1_s1d1, O_RDONLY));
	EXPECT_EQ(0, matches_log_fs(_metadata, self->audit_fd, "fs\\.read_file",
				    file1_s1d1));

	EXPECT_EQ(0, audit_count_records(self->audit_fd, &records));
	EXPECT_EQ(0, records.access);
	EXPECT_EQ(1, records.domain);
}

TEST_F(audit_layout1, read_dir)
{
	struct audit_records records;

	drop_access_rights(_metadata, &(struct landlock_ruleset_attr){
					      .handled_access_fs = access_fs_16,
				      });

	EXPECT_EQ(EACCES, test_open(dir_s1d1, O_DIRECTORY));
	EXPECT_EQ(0, matches_log_fs(_metadata, self->audit_fd, "fs\\.read_dir",
				    dir_s1d1));

	EXPECT_EQ(0, audit_count_records(self->audit_fd, &records));
	EXPECT_EQ(0, records.access);
	EXPECT_EQ(1, records.domain);
}

TEST_F(audit_layout1, remove_dir)
{
	struct audit_records records;

	EXPECT_EQ(0, unlink(file1_s1d3));
	EXPECT_EQ(0, unlink(file2_s1d3));

	drop_access_rights(_metadata, &(struct landlock_ruleset_attr){
					      .handled_access_fs = access_fs_16,
				      });

	EXPECT_EQ(-1, rmdir(dir_s1d3));
	EXPECT_EQ(EACCES, errno);
	EXPECT_EQ(0, matches_log_fs(_metadata, self->audit_fd,
				    "fs\\.remove_dir", dir_s1d2));

	EXPECT_EQ(-1, unlinkat(AT_FDCWD, dir_s1d3, AT_REMOVEDIR));
	EXPECT_EQ(EACCES, errno);
	EXPECT_EQ(0, matches_log_fs(_metadata, self->audit_fd,
				    "fs\\.remove_dir", dir_s1d2));

	EXPECT_EQ(0, audit_count_records(self->audit_fd, &records));
	EXPECT_EQ(0, records.access);
	EXPECT_EQ(0, records.domain);
}

TEST_F(audit_layout1, remove_file)
{
	struct audit_records records;

	drop_access_rights(_metadata, &(struct landlock_ruleset_attr){
					      .handled_access_fs = access_fs_16,
				      });

	EXPECT_EQ(-1, unlink(file1_s1d3));
	EXPECT_EQ(EACCES, errno);
	EXPECT_EQ(0, matches_log_fs(_metadata, self->audit_fd,
				    "fs\\.remove_file", dir_s1d3));

	EXPECT_EQ(0, audit_count_records(self->audit_fd, &records));
	EXPECT_EQ(0, records.access);
	EXPECT_EQ(1, records.domain);
}

TEST_F(audit_layout1, make_char)
{
	struct audit_records records;

	EXPECT_EQ(0, unlink(file1_s1d3));

	drop_access_rights(_metadata, &(struct landlock_ruleset_attr){
					      .handled_access_fs = access_fs_16,
				      });

	EXPECT_EQ(-1, mknod(file1_s1d3, S_IFCHR | 0644, 0));
	EXPECT_EQ(EACCES, errno);
	EXPECT_EQ(0, matches_log_fs(_metadata, self->audit_fd, "fs\\.make_char",
				    dir_s1d3));

	EXPECT_EQ(0, audit_count_records(self->audit_fd, &records));
	EXPECT_EQ(0, records.access);
	EXPECT_EQ(1, records.domain);
}

TEST_F(audit_layout1, make_dir)
{
	struct audit_records records;

	EXPECT_EQ(0, unlink(file1_s1d3));

	drop_access_rights(_metadata, &(struct landlock_ruleset_attr){
					      .handled_access_fs = access_fs_16,
				      });

	EXPECT_EQ(-1, mkdir(file1_s1d3, 0755));
	EXPECT_EQ(EACCES, errno);
	EXPECT_EQ(0, matches_log_fs(_metadata, self->audit_fd, "fs\\.make_dir",
				    dir_s1d3));

	EXPECT_EQ(0, audit_count_records(self->audit_fd, &records));
	EXPECT_EQ(0, records.access);
	EXPECT_EQ(1, records.domain);
}

TEST_F(audit_layout1, make_reg)
{
	struct audit_records records;

	EXPECT_EQ(0, unlink(file1_s1d3));

	drop_access_rights(_metadata, &(struct landlock_ruleset_attr){
					      .handled_access_fs = access_fs_16,
				      });

	EXPECT_EQ(-1, mknod(file1_s1d3, S_IFREG | 0644, 0));
	EXPECT_EQ(EACCES, errno);
	EXPECT_EQ(0, matches_log_fs(_metadata, self->audit_fd, "fs\\.make_reg",
				    dir_s1d3));

	EXPECT_EQ(0, audit_count_records(self->audit_fd, &records));
	EXPECT_EQ(0, records.access);
	EXPECT_EQ(1, records.domain);
}

TEST_F(audit_layout1, make_sock)
{
	struct audit_records records;

	EXPECT_EQ(0, unlink(file1_s1d3));

	drop_access_rights(_metadata, &(struct landlock_ruleset_attr){
					      .handled_access_fs = access_fs_16,
				      });

	EXPECT_EQ(-1, mknod(file1_s1d3, S_IFSOCK | 0644, 0));
	EXPECT_EQ(EACCES, errno);
	EXPECT_EQ(0, matches_log_fs(_metadata, self->audit_fd, "fs\\.make_sock",
				    dir_s1d3));

	EXPECT_EQ(0, audit_count_records(self->audit_fd, &records));
	EXPECT_EQ(0, records.access);
	EXPECT_EQ(1, records.domain);
}

TEST_F(audit_layout1, make_fifo)
{
	struct audit_records records;

	EXPECT_EQ(0, unlink(file1_s1d3));

	drop_access_rights(_metadata, &(struct landlock_ruleset_attr){
					      .handled_access_fs = access_fs_16,
				      });

	EXPECT_EQ(-1, mknod(file1_s1d3, S_IFIFO | 0644, 0));
	EXPECT_EQ(EACCES, errno);
	EXPECT_EQ(0, matches_log_fs(_metadata, self->audit_fd, "fs\\.make_fifo",
				    dir_s1d3));

	EXPECT_EQ(0, audit_count_records(self->audit_fd, &records));
	EXPECT_EQ(0, records.access);
	EXPECT_EQ(1, records.domain);
}

TEST_F(audit_layout1, make_block)
{
	struct audit_records records;

	EXPECT_EQ(0, unlink(file1_s1d3));

	drop_access_rights(_metadata, &(struct landlock_ruleset_attr){
					      .handled_access_fs = access_fs_16,
				      });

	EXPECT_EQ(-1, mknod(file1_s1d3, S_IFBLK | 0644, 0));
	EXPECT_EQ(EACCES, errno);
	EXPECT_EQ(0, matches_log_fs(_metadata, self->audit_fd,
				    "fs\\.make_block", dir_s1d3));

	EXPECT_EQ(0, audit_count_records(self->audit_fd, &records));
	EXPECT_EQ(0, records.access);
	EXPECT_EQ(1, records.domain);
}

TEST_F(audit_layout1, make_sym)
{
	struct audit_records records;

	EXPECT_EQ(0, unlink(file1_s1d3));

	drop_access_rights(_metadata, &(struct landlock_ruleset_attr){
					      .handled_access_fs = access_fs_16,
				      });

	EXPECT_EQ(-1, symlink("target", file1_s1d3));
	EXPECT_EQ(EACCES, errno);
	EXPECT_EQ(0, matches_log_fs(_metadata, self->audit_fd, "fs\\.make_sym",
				    dir_s1d3));

	EXPECT_EQ(0, audit_count_records(self->audit_fd, &records));
	EXPECT_EQ(0, records.access);
	EXPECT_EQ(1, records.domain);
}

TEST_F(audit_layout1, refer_handled)
{
	struct audit_records records;

	EXPECT_EQ(0, unlink(file1_s1d3));

	drop_access_rights(_metadata, &(struct landlock_ruleset_attr){
					      .handled_access_fs =
						      LANDLOCK_ACCESS_FS_REFER,
				      });

	EXPECT_EQ(-1, link(file1_s1d1, file1_s1d3));
	EXPECT_EQ(EXDEV, errno);
	EXPECT_EQ(0, matches_log_fs(_metadata, self->audit_fd, "fs\\.refer",
				    dir_s1d1));
	EXPECT_EQ(0,
		  matches_log_domain_allocated(self->audit_fd, getpid(), NULL));
	EXPECT_EQ(0, matches_log_fs(_metadata, self->audit_fd, "fs\\.refer",
				    dir_s1d3));

	EXPECT_EQ(0, audit_count_records(self->audit_fd, &records));
	EXPECT_EQ(0, records.access);
	EXPECT_EQ(0, records.domain);
}

TEST_F(audit_layout1, refer_make)
{
	struct audit_records records;

	EXPECT_EQ(0, unlink(file1_s1d3));

	drop_access_rights(_metadata,
			   &(struct landlock_ruleset_attr){
				   .handled_access_fs =
					   LANDLOCK_ACCESS_FS_MAKE_REG |
					   LANDLOCK_ACCESS_FS_REFER,
			   });

	EXPECT_EQ(-1, link(file1_s1d1, file1_s1d3));
	EXPECT_EQ(EACCES, errno);
	EXPECT_EQ(0, matches_log_fs(_metadata, self->audit_fd, "fs\\.refer",
				    dir_s1d1));
	EXPECT_EQ(0, matches_log_fs(_metadata, self->audit_fd,
				    "fs\\.make_reg,fs\\.refer", dir_s1d3));

	EXPECT_EQ(0, audit_count_records(self->audit_fd, &records));
	EXPECT_EQ(0, records.access);
	EXPECT_EQ(0, records.domain);
}

TEST_F(audit_layout1, refer_rename)
{
	struct audit_records records;

	EXPECT_EQ(0, unlink(file1_s1d3));

	drop_access_rights(_metadata, &(struct landlock_ruleset_attr){
					      .handled_access_fs = access_fs_16,
				      });

	EXPECT_EQ(EACCES, test_rename(file1_s1d2, file1_s2d3));
	EXPECT_EQ(0, matches_log_fs(_metadata, self->audit_fd,
				    "fs\\.remove_file,fs\\.refer", dir_s1d2));
	EXPECT_EQ(0, matches_log_fs(_metadata, self->audit_fd,
				    "fs\\.remove_file,fs\\.make_reg,fs\\.refer",
				    dir_s2d3));

	EXPECT_EQ(0, audit_count_records(self->audit_fd, &records));
	EXPECT_EQ(0, records.access);
	EXPECT_EQ(0, records.domain);
}

TEST_F(audit_layout1, refer_exchange)
{
	struct audit_records records;

	EXPECT_EQ(0, unlink(file1_s1d3));

	drop_access_rights(_metadata, &(struct landlock_ruleset_attr){
					      .handled_access_fs = access_fs_16,
				      });

	/*
	 * The only difference with the previous audit_layout1.refer_rename test is
	 * the extra ",fs\\.make_reg" blocked by the source directory.
	 */
	EXPECT_EQ(EACCES, test_exchange(file1_s1d2, file1_s2d3));
	EXPECT_EQ(0, matches_log_fs(_metadata, self->audit_fd,
				    "fs\\.remove_file,fs\\.make_reg,fs\\.refer",
				    dir_s1d2));
	EXPECT_EQ(0, matches_log_fs(_metadata, self->audit_fd,
				    "fs\\.remove_file,fs\\.make_reg,fs\\.refer",
				    dir_s2d3));

	EXPECT_EQ(0, audit_count_records(self->audit_fd, &records));
	EXPECT_EQ(0, records.access);
	EXPECT_EQ(0, records.domain);
}

/*
 * This test checks that the audit record is correctly generated when the
 * operation is only partially denied.  This is the case for rename(2) when the
 * source file is allowed to be referenced but the destination directory is not.
 *
 * This is also a regression test for commit d617f0d72d80 ("landlock: Optimize
 * file path walks and prepare for audit support") and commit 058518c20920
 * ("landlock: Align partial refer access checks with final ones").
 */
TEST_F(audit_layout1, refer_rename_half)
{
	struct audit_records records;
	const struct rule layer1[] = {
		{
			.path = dir_s2d2,
			.access = LANDLOCK_ACCESS_FS_REFER,
		},
		{},
	};
	int ruleset_fd =
		create_ruleset(_metadata, LANDLOCK_ACCESS_FS_REFER, layer1);

	ASSERT_LE(0, ruleset_fd);
	enforce_ruleset(_metadata, ruleset_fd);
	ASSERT_EQ(0, close(ruleset_fd));

	ASSERT_EQ(-1, rename(dir_s1d2, dir_s2d3));
	ASSERT_EQ(EXDEV, errno);

	/* Only half of the request is denied. */
	EXPECT_EQ(0, matches_log_fs(_metadata, self->audit_fd, "fs\\.refer",
				    dir_s1d1));

	EXPECT_EQ(0, audit_count_records(self->audit_fd, &records));
	EXPECT_EQ(0, records.access);
	EXPECT_EQ(1, records.domain);
}

TEST_F(audit_layout1, truncate)
{
	struct audit_records records;

	drop_access_rights(_metadata, &(struct landlock_ruleset_attr){
					      .handled_access_fs = access_fs_16,
				      });

	EXPECT_EQ(-1, truncate(file1_s1d3, 0));
	EXPECT_EQ(EACCES, errno);
	EXPECT_EQ(0, matches_log_fs(_metadata, self->audit_fd, "fs\\.truncate",
				    file1_s1d3));

	EXPECT_EQ(0, audit_count_records(self->audit_fd, &records));
	EXPECT_EQ(0, records.access);
	EXPECT_EQ(1, records.domain);
}

TEST_F(audit_layout1, ioctl_dev)
{
	struct audit_records records;
	int fd;

	drop_access_rights(_metadata,
			   &(struct landlock_ruleset_attr){
				   .handled_access_fs =
					   access_fs_16 &
					   ~LANDLOCK_ACCESS_FS_READ_FILE,
			   });

	fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
	ASSERT_LE(0, fd);
	EXPECT_EQ(EACCES, ioctl_error(_metadata, fd, FIONREAD));
	EXPECT_EQ(0, matches_log_fs_extra(_metadata, self->audit_fd,
					  "fs\\.ioctl_dev", "/dev/null",
					  " ioctlcmd=0x541b"));

	EXPECT_EQ(0, audit_count_records(self->audit_fd, &records));
	EXPECT_EQ(0, records.access);
	EXPECT_EQ(1, records.domain);
}

TEST_F(audit_layout1, mount)
{
	struct audit_records records;

	drop_access_rights(_metadata,
			   &(struct landlock_ruleset_attr){
				   .handled_access_fs =
					   LANDLOCK_ACCESS_FS_EXECUTE,
			   });

	set_cap(_metadata, CAP_SYS_ADMIN);
	EXPECT_EQ(-1, mount(NULL, dir_s3d2, NULL, MS_RDONLY, NULL));
	EXPECT_EQ(EPERM, errno);
	clear_cap(_metadata, CAP_SYS_ADMIN);
	EXPECT_EQ(0, matches_log_fs(_metadata, self->audit_fd,
				    "fs\\.change_topology", dir_s3d2));
	EXPECT_EQ(0, audit_count_records(self->audit_fd, &records));
	EXPECT_EQ(0, records.access);
	EXPECT_EQ(1, records.domain);
}

TEST_HARNESS_MAIN
