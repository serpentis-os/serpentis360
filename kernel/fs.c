#include "kernel.h"
#include "ppc.h"
#include "block.h"
#include "fs.h"

// serpentis-os fs v5, same bytes on disk as the x86 one
//   2048            superblock
//   2049..3072      entries, one sector each
//   3073..3104      bitmap, one bit per data sector
//   3105..131071    data

#define FS_MAGIC 0x53525033
#define FS_VERSION 5
#define SUPER_LBA 2048
#define ENTRIES_LBA 2049
#define BITMAP_LBA (ENTRIES_LBA + FS_MAX_FILES)
#define BITMAP_SECTORS 32
#define DATA_LBA (BITMAP_LBA + BITMAP_SECTORS)
#define DATA_SECTORS (FS_DISK_SECTORS - DATA_LBA)
#define EXTENT_MAX 32

#define E_FLAGS 0
#define E_EXTENTS 1
#define E_PARENT 2
#define E_SIZE 4
#define E_MTIME 8
#define E_SECTORS 12
#define E_PATH 16
#define E_OWNER 248
#define E_MODE 250
#define E_EXTENT 256
#define FLAG_USED 1
#define FLAG_DIR 2
#define MODE_FILE 0644
#define MODE_DIR 0755

static uint8_t entries[FS_MAX_FILES][512];
static uint8_t bitmap[BITMAP_SECTORS * 512];
static uint8_t bitmap_dirty[BITMAP_SECTORS];
static int dev = -1;
static int cwd;
static char cwd_path[FS_PATH_MAX] = "/";
static uint8_t file_buf[FS_FILE_MAX];
static volatile int fs_lock;

static const char *messages[] = {
	"ok", "No such file or directory", "File exists", "Not a directory", "Is a directory",
	"Directory not empty", "No space left", "Bad name", "Permission denied", "Disk error",
	"File too big", "No filesystem", "Busy"
};

const char *fs_error(int code)
{
	if (code > 0 || -code >= (int)(sizeof messages / sizeof *messages))
		return "Unknown error";
	return messages[-code];
}

static uint16_t g16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t g32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void p16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void p32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static int used(int i) { return entries[i][E_FLAGS] & FLAG_USED; }
static int is_dir(int i) { return entries[i][E_FLAGS] & FLAG_DIR; }
static const char *path_of(int i) { return (const char *)&entries[i][E_PATH]; }

static int save_entry(int i)
{
	return block_write(dev, ENTRIES_LBA + i, 1, entries[i]) < 0 ? FS_EIO : FS_OK;
}

static void bit_set(uint32_t n, int on)
{
	if (on)
		bitmap[n / 8] |= 1 << (n % 8);
	else
		bitmap[n / 8] &= ~(1 << (n % 8));
	bitmap_dirty[n / 8 / 512] = 1;
}

static int bit_get(uint32_t n)
{
	return bitmap[n / 8] >> (n % 8) & 1;
}

static int save_bitmap(void)
{
	for (int s = 0; s < BITMAP_SECTORS; s++)
		if (bitmap_dirty[s]) {
			if (block_write(dev, BITMAP_LBA + s, 1, bitmap + s * 512) < 0)
				return FS_EIO;
			bitmap_dirty[s] = 0;
		}
	return FS_OK;
}

// 2026 until there is an rtc driver, then real time. packed the serpentis-os way
uint32_t fs_now(void)
{
	uint64_t s = ticks / HZ;
	uint32_t year = 26, month = 1, day = 1 + s / 86400 % 28;
	uint32_t hour = s / 3600 % 24, min = s / 60 % 60, sec = s % 60;
	return ((((year & 63) << 4 | month) << 5 | day) << 5 | hour) << 12 | min << 6 | sec;
}

void fs_date(uint32_t t, char *out, int room)
{
	snprintf(out, room, "%04d-%02d-%02d %02d:%02d", 2000 + (t >> 26), t >> 22 & 15, t >> 17 & 31,
		 t >> 12 & 31, t >> 6 & 63);
}

int fs_device(void)
{
	return dev;
}

int fs_mount(int d)
{
	uint8_t super[512];
	if (block_read(d, SUPER_LBA, 1, super) < 0)
		return FS_EIO;
	if (g32(super) != FS_MAGIC || g16(super + 4) != FS_VERSION)
		return FS_ENOFS;
	struct block *b = block_get(d);
	if (!b || b->sectors < FS_DISK_SECTORS)
		return FS_ENOFS;
	if (block_read(d, ENTRIES_LBA, FS_MAX_FILES, entries) < 0 ||
	    block_read(d, BITMAP_LBA, BITMAP_SECTORS, bitmap) < 0)
		return FS_EIO;
	dev = d;
	cwd = 0;
	strcpy(cwd_path, "/");
	return FS_OK;
}

void fs_unmount(void)
{
	dev = -1;
}

int fs_format(int d)
{
	struct block *b = block_get(d);
	if (!b || b->sectors < FS_DISK_SECTORS)
		return FS_ENOSPC;
	uint8_t super[512] = {0};
	p32(super, FS_MAGIC);
	p16(super + 4, FS_VERSION);
	p16(super + 6, 512);
	p16(super + 8, FS_MAX_FILES);
	p32(super + 10, DATA_LBA);
	p32(super + 14, DATA_SECTORS);
	p32(super + 18, BITMAP_LBA);
	p16(super + 22, BITMAP_SECTORS);
	memset(entries, 0, sizeof entries);
	memset(bitmap, 0, sizeof bitmap);
	entries[0][E_FLAGS] = FLAG_USED | FLAG_DIR;
	p32(entries[0] + E_MTIME, fs_now());
	strcpy((char *)&entries[0][E_PATH], "/");
	p16(entries[0] + E_MODE, MODE_DIR);
	if (block_write(d, SUPER_LBA, 1, super) < 0 || block_write(d, ENTRIES_LBA, FS_MAX_FILES, entries) < 0 ||
	    block_write(d, BITMAP_LBA, BITMAP_SECTORS, bitmap) < 0)
		return FS_EIO;
	memset(bitmap_dirty, 0, sizeof bitmap_dirty);
	dev = d;
	cwd = 0;
	strcpy(cwd_path, "/");
	return FS_OK;
}

int fs_resolve(const char *path, char *out)
{
	char tmp[FS_PATH_MAX * 2];
	if (path[0] == '/')
		snprintf(tmp, sizeof tmp, "%s", path);
	else
		snprintf(tmp, sizeof tmp, "%s/%s", cwd_path, path);
	char *parts[64];
	int n = 0;
	for (char *s = tmp; *s;) {
		while (*s == '/')
			*s++ = 0;
		if (!*s)
			break;
		char *start = s;
		while (*s && *s != '/')
			s++;
		if (*s)
			*s++ = 0;
		if (!strcmp(start, "."))
			continue;
		if (!strcmp(start, "..")) {
			if (n)
				n--;
			continue;
		}
		if (n == 64)
			return FS_ENAME;
		parts[n++] = start;
	}
	int len = 0;
	out[0] = '/';
	out[1] = 0;
	for (int i = 0; i < n; i++) {
		int l = strlen(parts[i]);
		if (len + 1 + l >= FS_PATH_MAX)
			return FS_ENAME;
		out[len++] = '/';
		memcpy(out + len, parts[i], l);
		len += l;
		out[len] = 0;
	}
	if (!len)
		out[1] = 0;
	return FS_OK;
}

static int find(const char *abs)
{
	for (int i = 0; i < FS_MAX_FILES; i++)
		if (used(i) && !strcmp(path_of(i), abs))
			return i;
	return FS_ENOENT;
}

static int lookup(const char *path)
{
	char abs[FS_PATH_MAX];
	int r = fs_resolve(path, abs);
	return r < 0 ? r : find(abs);
}

static void parent_of(const char *abs, char *out)
{
	strcpy(out, abs);
	char *slash = strrchr(out, '/');
	if (slash == out)
		out[1] = 0;
	else
		*slash = 0;
}

static const char *base_of(const char *abs)
{
	return strrchr(abs, '/') + 1;
}

// like access.asm: root may do anything, the owner gets the high bits, everyone else the low
static int may(int i, int bit)
{
	if (user_is_root())
		return 1;
	int mode = g16(entries[i] + E_MODE);
	return g16(entries[i] + E_OWNER) == user_uid() ? (mode >> 6) & bit : mode & bit;
}

static void release(int i)
{
	int n = entries[i][E_EXTENTS];
	for (int k = 0; k < n; k++) {
		const uint8_t *x = entries[i] + E_EXTENT + k * 8;
		uint32_t lba = g32(x), count = g32(x + 4);
		for (uint32_t s = 0; s < count; s++)
			if (lba + s >= DATA_LBA)
				bit_set(lba + s - DATA_LBA, 0);
	}
	entries[i][E_EXTENTS] = 0;
	p32(entries[i] + E_SECTORS, 0);
	memset(entries[i] + E_EXTENT, 0, EXTENT_MAX * 8);
}

// TODO walks the whole bitmap for every extent, slow once the disk fills up
static int allocate(int i, uint32_t sectors)
{
	int n = 0;
	uint32_t want = sectors;
	while (want) {
		uint32_t best = 0, best_len = 0, run = 0, start = 0;
		for (uint32_t s = 0; s <= DATA_SECTORS; s++) {
			if (s < DATA_SECTORS && !bit_get(s)) {
				if (!run)
					start = s;
				run++;
				if (run >= want) {
					best = start;
					best_len = want;
					break;
				}
			} else {
				if (run > best_len) {
					best = start;
					best_len = run;
				}
				run = 0;
			}
		}
		if (!best_len || n == EXTENT_MAX) {
			release(i);
			return FS_ENOSPC;
		}
		uint32_t take = best_len < want ? best_len : want;
		for (uint32_t s = 0; s < take; s++)
			bit_set(best + s, 1);
		p32(entries[i] + E_EXTENT + n * 8, DATA_LBA + best);
		p32(entries[i] + E_EXTENT + n * 8 + 4, take);
		n++;
		entries[i][E_EXTENTS] = n;
		want -= take;
	}
	p32(entries[i] + E_SECTORS, sectors);
	return FS_OK;
}

static int transfer(int i, uint8_t *buf, int write)
{
	int n = entries[i][E_EXTENTS];
	uint8_t *p = buf;
	for (int k = 0; k < n; k++) {
		const uint8_t *x = entries[i] + E_EXTENT + k * 8;
		uint32_t lba = g32(x), count = g32(x + 4);
		int r = write ? block_write(dev, lba, count, p) : block_read(dev, lba, count, p);
		if (r < 0)
			return FS_EIO;
		p += (uint64_t)count * 512;
	}
	return FS_OK;
}

static int free_slot(void)
{
	for (int i = 1; i < FS_MAX_FILES; i++)
		if (!used(i))
			return i;
	return FS_ENOSPC;
}

static int create(const char *path, int dir)
{
	char abs[FS_PATH_MAX], parent[FS_PATH_MAX];
	int r = fs_resolve(path, abs);
	if (r < 0)
		return r;
	if (!strcmp(abs, "/"))
		return FS_EEXIST;
	const char *base = base_of(abs);
	if (!*base || strchr(base, ':'))
		return FS_ENAME;
	if (find(abs) >= 0)
		return FS_EEXIST;
	parent_of(abs, parent);
	int p = find(parent);
	if (p < 0)
		return FS_ENOENT;
	if (!is_dir(p))
		return FS_ENOTDIR;
	if (!may(p, 2))
		return FS_EACCES;
	int i = free_slot();
	if (i < 0)
		return i;
	memset(entries[i], 0, 512);
	entries[i][E_FLAGS] = FLAG_USED | (dir ? FLAG_DIR : 0);
	p16(entries[i] + E_PARENT, p);
	p32(entries[i] + E_MTIME, fs_now());
	strcpy((char *)&entries[i][E_PATH], abs);
	p16(entries[i] + E_OWNER, user_uid() < 0 ? 0 : user_uid());
	p16(entries[i] + E_MODE, dir ? MODE_DIR : MODE_FILE);
	r = save_entry(i);
	return r < 0 ? r : i;
}

// the shell and setup can both write, so anything that changes the table takes the lock
#define LOCKED(expr) ({ if (dev < 0) return FS_ENOFS; lock_take(&fs_lock); int r_ = (expr); lock_give(&fs_lock); r_; })

static int do_mkdir(const char *path)
{
	int i = create(path, 1);
	return i < 0 ? i : FS_OK;
}

int fs_mkdir(const char *path)
{
	return LOCKED(do_mkdir(path));
}

static int do_touch(const char *path)
{
	int i = lookup(path);
	if (i >= 0) {
		if (!may(i, 2))
			return FS_EACCES;
		p32(entries[i] + E_MTIME, fs_now());
		return save_entry(i);
	}
	i = create(path, 0);
	return i < 0 ? i : FS_OK;
}

int fs_touch(const char *path)
{
	return LOCKED(do_touch(path));
}

static int do_remove(const char *path, int want_dir)
{
	int i = lookup(path);
	if (i < 0)
		return i;
	if (i == 0)
		return FS_EACCES;
	if (want_dir && !is_dir(i))
		return FS_ENOTDIR;
	if (!want_dir && is_dir(i))
		return FS_EISDIR;
	int p = g16(entries[i] + E_PARENT);
	if (!may(p, 2) || !may(i, 2))
		return FS_EACCES;
	if (want_dir) {
		if (i == cwd)
			return FS_EBUSY;
		for (int k = 1; k < FS_MAX_FILES; k++)
			if (used(k) && g16(entries[k] + E_PARENT) == i)
				return FS_ENOTEMPTY;
	}
	release(i);
	memset(entries[i], 0, 512);
	int r = save_entry(i);
	int b = save_bitmap();
	return r < 0 ? r : b;
}

int fs_remove(const char *path)
{
	return LOCKED(do_remove(path, 0));
}

int fs_rmdir(const char *path)
{
	return LOCKED(do_remove(path, 1));
}

static void fill_stat(int i, struct fs_stat *st)
{
	st->index = i;
	st->dir = is_dir(i) != 0;
	st->size = g32(entries[i] + E_SIZE);
	st->mtime = g32(entries[i] + E_MTIME);
	st->owner = g16(entries[i] + E_OWNER);
	st->mode = g16(entries[i] + E_MODE);
	st->sectors = g32(entries[i] + E_SECTORS);
	st->extents = entries[i][E_EXTENTS];
	st->parent = g16(entries[i] + E_PARENT);
}

int fs_stat(const char *path, struct fs_stat *st)
{
	if (dev < 0)
		return FS_ENOFS;
	int i = lookup(path);
	if (i < 0)
		return i;
	fill_stat(i, st);
	return FS_OK;
}

static int do_read(const char *path, void *buf, int room)
{
	int i = lookup(path);
	if (i < 0)
		return i;
	if (is_dir(i))
		return FS_EISDIR;
	if (!may(i, 4))
		return FS_EACCES;
	uint32_t size = g32(entries[i] + E_SIZE);
	if ((int)size > room)
		return FS_EBIG;
	int r = transfer(i, file_buf, 0);
	if (r < 0)
		return r;
	memcpy(buf, file_buf, size);
	return size;
}

int fs_read(const char *path, void *buf, int room)
{
	return LOCKED(do_read(path, buf, room));
}

// every write puts the whole file down again, release then allocate. dumb but files are
// at most 1 MB and it keeps the extents simple
static int do_write(const char *path, const void *data, int size)
{
	if (size > FS_FILE_MAX)
		return FS_EBIG;
	int i = lookup(path);
	if (i < 0) {
		i = create(path, 0);
		if (i < 0)
			return i;
	} else if (is_dir(i)) {
		return FS_EISDIR;
	} else if (!may(i, 2)) {
		return FS_EACCES;
	}
	release(i);
	uint32_t sectors = (size + 511) / 512;
	int r = allocate(i, sectors);
	if (r < 0) {
		p32(entries[i] + E_SIZE, 0);
		save_entry(i);
		save_bitmap();
		return r;
	}
	memset(file_buf, 0, (size_t)sectors * 512);
	memcpy(file_buf, data, size);
	r = transfer(i, file_buf, 1);
	p32(entries[i] + E_SIZE, size);
	p32(entries[i] + E_MTIME, fs_now());
	int e = save_entry(i);
	int b = save_bitmap();
	return r < 0 ? r : e < 0 ? e : b;
}

int fs_write(const char *path, const void *data, int size)
{
	return LOCKED(do_write(path, data, size));
}

static uint8_t append_buf[FS_FILE_MAX];

// FIXME reads and writes the whole file, a log that keeps growing gets slow
int fs_append(const char *path, const void *data, int size)
{
	if (dev < 0)
		return FS_ENOFS;
	int old = fs_read(path, append_buf, FS_FILE_MAX);
	if (old == FS_ENOENT)
		old = 0;
	else if (old < 0)
		return old;
	if (old + size > FS_FILE_MAX)
		return FS_EBIG;
	memcpy(append_buf + old, data, size);
	return fs_write(path, append_buf, old + size);
}

// paths are stored in full so moving a directory renames everything under it.
// TODO if cwd is inside the moved directory cwd_path keeps the old name
static int do_rename(const char *from, const char *to)
{
	char src[FS_PATH_MAX], dst[FS_PATH_MAX], parent[FS_PATH_MAX];
	int r = fs_resolve(from, src);
	if (r < 0)
		return r;
	r = fs_resolve(to, dst);
	if (r < 0)
		return r;
	int i = find(src);
	if (i < 0)
		return i;
	if (i == 0)
		return FS_EACCES;
	int t = find(dst);
	if (t >= 0 && is_dir(t)) {
		int l = strlen(dst);
		snprintf(dst + l, sizeof dst - l, "%s%s", l > 1 ? "/" : "", base_of(src));
		t = find(dst);
	}
	if (t >= 0)
		return FS_EEXIST;
	int sl = strlen(src);
	if (!strncmp(dst, src, sl) && (dst[sl] == '/' || !dst[sl]))
		return FS_EACCES;
	parent_of(dst, parent);
	int p = find(parent);
	if (p < 0)
		return FS_ENOENT;
	if (!is_dir(p))
		return FS_ENOTDIR;
	if (!may(p, 2) || !may(g16(entries[i] + E_PARENT), 2))
		return FS_EACCES;
	int dl = strlen(dst);
	for (int k = 0; k < FS_MAX_FILES; k++) {
		if (!used(k) || k == i)
			continue;
		const char *pk = path_of(k);
		if (strncmp(pk, src, sl) || pk[sl] != '/')
			continue;
		if (dl + (int)strlen(pk + sl) >= FS_PATH_MAX)
			return FS_ENAME;
	}
	for (int k = 0; k < FS_MAX_FILES; k++) {
		if (!used(k) || k == i)
			continue;
		char *pk = (char *)&entries[k][E_PATH];
		if (strncmp(pk, src, sl) || pk[sl] != '/')
			continue;
		char rest[FS_PATH_MAX];
		strcpy(rest, pk + sl);
		snprintf(pk, FS_PATH_MAX, "%s%s", dst, rest);
		save_entry(k);
	}
	strcpy((char *)&entries[i][E_PATH], dst);
	p16(entries[i] + E_PARENT, p);
	if (i == cwd)
		strcpy(cwd_path, dst);
	return save_entry(i);
}

int fs_rename(const char *from, const char *to)
{
	return LOCKED(do_rename(from, to));
}

int fs_readdir(const char *path, int *cursor, char *name, struct fs_stat *st)
{
	if (dev < 0)
		return FS_ENOFS;
	int d = lookup(path);
	if (d < 0)
		return d;
	if (!is_dir(d))
		return FS_ENOTDIR;
	if (!may(d, 4))
		return FS_EACCES;
	for (int i = *cursor; i < FS_MAX_FILES; i++) {
		if (i == 0 || !used(i) || g16(entries[i] + E_PARENT) != d)
			continue;
		*cursor = i + 1;
		strcpy(name, base_of(path_of(i)));
		fill_stat(i, st);
		return 1;
	}
	*cursor = FS_MAX_FILES;
	return 0;
}

int fs_chdir(const char *path)
{
	if (dev < 0)
		return FS_ENOFS;
	char abs[FS_PATH_MAX];
	int r = fs_resolve(path, abs);
	if (r < 0)
		return r;
	int i = find(abs);
	if (i < 0)
		return i;
	if (!is_dir(i))
		return FS_ENOTDIR;
	if (!may(i, 1))
		return FS_EACCES;
	cwd = i;
	strcpy(cwd_path, abs);
	return FS_OK;
}

const char *fs_cwd(void)
{
	return cwd_path;
}

int fs_chmod(const char *path, int mode)
{
	if (dev < 0)
		return FS_ENOFS;
	int i = lookup(path);
	if (i < 0)
		return i;
	if (!user_is_root() && g16(entries[i] + E_OWNER) != user_uid())
		return FS_EACCES;
	p16(entries[i] + E_MODE, mode & 0777);
	return save_entry(i);
}

int fs_chown(const char *path, int uid)
{
	if (dev < 0)
		return FS_ENOFS;
	int i = lookup(path);
	if (i < 0)
		return i;
	if (!user_is_root())
		return FS_EACCES;
	p16(entries[i] + E_OWNER, uid);
	return save_entry(i);
}

void fs_usage(uint32_t *used_sectors, uint32_t *total, int *files)
{
	*used_sectors = 0;
	*files = 0;
	for (uint32_t s = 0; s < DATA_SECTORS; s++)
		*used_sectors += bit_get(s);
	for (int i = 0; i < FS_MAX_FILES; i++)
		*files += used(i) != 0;
	*total = DATA_SECTORS;
}

int fs_check(int fix, void (*report)(const char *msg))
{
	static uint8_t seen[BITMAP_SECTORS * 512];
	char msg[300];
	int problems = 0;
	memset(seen, 0, sizeof seen);
	for (int i = 1; i < FS_MAX_FILES; i++) {
		if (!used(i))
			continue;
		int p = g16(entries[i] + E_PARENT);
		char parent[FS_PATH_MAX];
		parent_of(path_of(i), parent);
		if (p >= FS_MAX_FILES || !used(p) || !is_dir(p) || strcmp(path_of(p), parent)) {
			snprintf(msg, sizeof msg, "%s: parent is wrong", path_of(i));
			report(msg);
			problems++;
			int real = find(parent);
			if (fix && real >= 0 && is_dir(real)) {
				p16(entries[i] + E_PARENT, real);
				save_entry(i);
			}
		}
		for (int k = 0; k < entries[i][E_EXTENTS] && k < EXTENT_MAX; k++) {
			uint32_t lba = g32(entries[i] + E_EXTENT + k * 8), count = g32(entries[i] + E_EXTENT + k * 8 + 4);
			for (uint32_t s = 0; s < count; s++) {
				uint32_t n = lba + s - DATA_LBA;
				if (lba + s < DATA_LBA || n >= DATA_SECTORS) {
					snprintf(msg, sizeof msg, "%s: extent outside the disk", path_of(i));
					report(msg);
					problems++;
					break;
				}
				if (seen[n / 8] >> (n % 8) & 1) {
					snprintf(msg, sizeof msg, "%s: shares sector %u", path_of(i), lba + s);
					report(msg);
					problems++;
				}
				seen[n / 8] |= 1 << (n % 8);
			}
		}
	}
	int leaked = 0, lost = 0;
	for (uint32_t s = 0; s < DATA_SECTORS; s++) {
		int in_map = bit_get(s), in_use = seen[s / 8] >> (s % 8) & 1;
		if (in_map && !in_use)
			leaked++;
		if (!in_map && in_use)
			lost++;
		if (fix && in_map != in_use)
			bit_set(s, in_use);
	}
	if (leaked) {
		snprintf(msg, sizeof msg, "%d sectors marked used but no file has them", leaked);
		report(msg);
		problems++;
	}
	if (lost) {
		snprintf(msg, sizeof msg, "%d sectors in files but marked free", lost);
		report(msg);
		problems++;
	}
	if (fix)
		save_bitmap();
	return problems;
}
