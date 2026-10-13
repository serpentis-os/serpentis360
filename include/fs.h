#ifndef FS_H
#define FS_H

#include <stdint.h>

#define FS_PATH_MAX 232
#define FS_FILE_MAX 0x100000
#define FS_MAX_FILES 1024
// 64 MB, the size serpentis-os formats. bigger partitions work, the rest is just not used
#define FS_DISK_SECTORS 131072

enum {
	FS_OK = 0,
	FS_ENOENT = -1,
	FS_EEXIST = -2,
	FS_ENOTDIR = -3,
	FS_EISDIR = -4,
	FS_ENOTEMPTY = -5,
	FS_ENOSPC = -6,
	FS_ENAME = -7,
	FS_EACCES = -8,
	FS_EIO = -9,
	FS_EBIG = -10,
	FS_ENOFS = -11,
	FS_EBUSY = -12
};

struct fs_stat {
	int index;
	int dir;
	uint32_t size;
	uint32_t mtime;
	int owner;
	int mode;
	uint32_t sectors;
	int extents;
	int parent;
};

const char *fs_error(int code);
int fs_mount(int dev);
int fs_format(int dev);
void fs_unmount(void);
int fs_device(void);
int fs_resolve(const char *path, char *out);
int fs_stat(const char *path, struct fs_stat *st);
int fs_mkdir(const char *path);
int fs_touch(const char *path);
int fs_remove(const char *path);
int fs_rmdir(const char *path);
int fs_read(const char *path, void *buf, int room);
int fs_write(const char *path, const void *data, int size);
int fs_append(const char *path, const void *data, int size);
int fs_rename(const char *from, const char *to);
int fs_readdir(const char *path, int *cursor, char *name, struct fs_stat *st);
int fs_chdir(const char *path);
const char *fs_cwd(void);
int fs_chmod(const char *path, int mode);
int fs_chown(const char *path, int uid);
void fs_usage(uint32_t *used, uint32_t *total, int *files);
int fs_check(int fix, void (*report)(const char *msg));
uint32_t fs_now(void);
void fs_date(uint32_t packed, char *out, int room);

// these live in users.c, the fs needs them for the permission checks
int user_uid(void);
int user_is_root(void);
void user_sudo(int on);

#endif
