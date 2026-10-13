#include "kernel.h"
#include "ppc.h"
#include "screen.h"
#include "block.h"
#include "shell.h"

// most of these read the whole file into data[] and write it back. files are 1 MB at most
// so thats fine and a lot simpler than streaming
static char data[FS_FILE_MAX];

// takes a flag out of argv wherever it is, so rm file -r works too
static int has_flag(int *argc, char **argv, const char *flag)
{
	for (int i = 1; i < *argc; i++)
		if (!strcmp(argv[i], flag)) {
			for (int k = i; k < *argc - 1; k++)
				argv[k] = argv[k + 1];
			(*argc)--;
			return 1;
		}
	return 0;
}

static void join(char *out, const char *dir, const char *name)
{
	snprintf(out, FS_PATH_MAX, "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", name);
}

static void mode_text(const struct fs_stat *st, char *out)
{
	out[0] = st->dir ? 'd' : '-';
	for (int i = 0; i < 9; i++)
		out[1 + i] = st->mode & (0400 >> i) ? "rwx"[i % 3] : '-';
	out[10] = 0;
}

static void ls_one(const char *name, const struct fs_stat *st, int is_long)
{
	if (is_long) {
		char mode[11], owner[32], date[32];
		mode_text(st, mode);
		users_name_of(st->owner, owner, sizeof owner);
		fs_date(st->mtime, date, sizeof date);
		out("%s %-8s %7u %s ", mode, owner, st->size, date);
	}
	out_color(st->dir ? LIGHT_BLUE : WHITE);
	out("%s", name);
	out_color(WHITE);
	out(is_long ? "\n" : "  ");
}

static void cmd_ls(int argc, char **argv)
{
	int is_long = 0, all = 0;
	const char *path = ".";
	for (int i = 1; i < argc; i++) {
		if (argv[i][0] == '-' && argv[i][1]) {
			is_long |= strchr(argv[i], 'l') != NULL;
			all |= strchr(argv[i], 'a') != NULL;
		} else {
			path = argv[i];
		}
	}
	struct fs_stat st;
	int r = fs_stat(path, &st);
	if (r < 0) {
		sh_fs_error(path, r);
		return;
	}
	if (!st.dir) {
		ls_one(path, &st, is_long);
		if (!is_long)
			out("\n");
		return;
	}
	char name[FS_PATH_MAX];
	int cursor = 0, shown = 0;
	while ((r = fs_readdir(path, &cursor, name, &st)) > 0) {
		if (name[0] == '.' && !all)
			continue;
		ls_one(name, &st, is_long);
		shown++;
	}
	if (r < 0)
		sh_fs_error(path, r);
	else if (!is_long && shown)
		out("\n");
}

static void cmd_cd(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : users_current()->home;
	int r = fs_chdir(path);
	if (r < 0)
		sh_fs_error(path, r);
}

static int mkdir_p(const char *path)
{
	char abs[FS_PATH_MAX], part[FS_PATH_MAX];
	int r = fs_resolve(path, abs);
	if (r < 0)
		return r;
	for (int i = 1; abs[i - 1]; i++)
		if (abs[i] == '/' || !abs[i]) {
			memcpy(part, abs, i);
			part[i] = 0;
			r = fs_mkdir(part);
			if (r < 0 && r != FS_EEXIST)
				return r;
		}
	return FS_OK;
}

static void cmd_mkdir(int argc, char **argv)
{
	int parents = has_flag(&argc, argv, "-p");
	if (argc < 2) {
		sh_error("Usage: mkdir [-p] <dir>...");
		return;
	}
	for (int i = 1; i < argc; i++) {
		int r = parents ? mkdir_p(argv[i]) : fs_mkdir(argv[i]);
		if (r < 0)
			sh_fs_error(argv[i], r);
	}
}

// readdir starts over every round, the entry that was just deleted would throw the
// cursor off otherwise
static int remove_tree(const char *path)
{
	struct fs_stat st;
	int r = fs_stat(path, &st);
	if (r < 0)
		return r;
	if (!st.dir)
		return fs_remove(path);
	char name[FS_PATH_MAX], child[FS_PATH_MAX];
	for (;;) {
		int cursor = 0;
		r = fs_readdir(path, &cursor, name, &st);
		if (r <= 0)
			break;
		join(child, path, name);
		r = remove_tree(child);
		if (r < 0)
			return r;
	}
	return fs_rmdir(path);
}

static void cmd_rmdir(int argc, char **argv)
{
	int recursive = has_flag(&argc, argv, "-r");
	if (argc < 2) {
		sh_error("Usage: rmdir [-r] <dir>...");
		return;
	}
	for (int i = 1; i < argc; i++) {
		char abs[FS_PATH_MAX];
		fs_resolve(argv[i], abs);
		int r = recursive ? remove_tree(abs) : fs_rmdir(abs);
		if (r < 0)
			sh_fs_error(argv[i], r);
	}
}

static void cmd_rm(int argc, char **argv)
{
	int recursive = 0, force = 0;
	for (int i = 1; i < argc;) {
		if (argv[i][0] == '-' && argv[i][1]) {
			recursive |= strchr(argv[i], 'r') != NULL;
			force |= strchr(argv[i], 'f') != NULL;
			for (int k = i; k < argc - 1; k++)
				argv[k] = argv[k + 1];
			argc--;
		} else {
			i++;
		}
	}
	if (argc < 2) {
		sh_error("Usage: rm [-r] [-f] <file>...");
		return;
	}
	for (int i = 1; i < argc; i++) {
		char abs[FS_PATH_MAX];
		fs_resolve(argv[i], abs);
		int r = recursive ? remove_tree(abs) : fs_remove(abs);
		if (r < 0 && !(force && r == FS_ENOENT))
			sh_fs_error(argv[i], r);
	}
}

static void cmd_touch(int argc, char **argv)
{
	if (argc < 2) {
		sh_error("Usage: touch <file>...");
		return;
	}
	for (int i = 1; i < argc; i++) {
		int r = fs_touch(argv[i]);
		if (r < 0)
			sh_fs_error(argv[i], r);
	}
}

static int joined(int argc, char **argv, int from, char *buf, int room)
{
	int len = 0;
	for (int i = from; i < argc; i++)
		len += snprintf(buf + len, room - len, i > from ? " %s" : "%s", argv[i]);
	len += snprintf(buf + len, room - len, "\n");
	return len;
}

static void cmd_write(int argc, char **argv)
{
	if (argc < 2) {
		sh_error("Usage: write <file> <text>");
		return;
	}
	int len = joined(argc, argv, 2, data, sizeof data);
	int r = fs_write(argv[1], data, len);
	if (r < 0)
		sh_fs_error(argv[1], r);
}

static void cmd_append(int argc, char **argv)
{
	if (argc < 2) {
		sh_error("Usage: append <file> <text>");
		return;
	}
	int len = joined(argc, argv, 2, data, sizeof data);
	int r = fs_append(argv[1], data, len);
	if (r < 0)
		sh_fs_error(argv[1], r);
}

static void cmd_cat(int argc, char **argv)
{
	int number = has_flag(&argc, argv, "-n");
	int n = sh_input(argc, argv, 1, data, sizeof data);
	if (n <= 0)
		return;
	if (!number) {
		out_write(data, n);
		return;
	}
	int line = 1;
	for (int i = 0; i < n;) {
		int e = i;
		while (e < n && data[e] != '\n')
			e++;
		out("%6d\t", line++);
		out_write(data + i, e - i);
		out("\n");
		i = e + 1;
	}
}

static int copy_file(const char *from, const char *to)
{
	int n = fs_read(from, data, sizeof data);
	if (n < 0)
		return n;
	return fs_write(to, data, n);
}

static int copy_tree(const char *from, const char *to)
{
	struct fs_stat st;
	int r = fs_stat(from, &st);
	if (r < 0)
		return r;
	if (!st.dir)
		return copy_file(from, to);
	r = fs_mkdir(to);
	if (r < 0 && r != FS_EEXIST)
		return r;
	char name[FS_PATH_MAX], a[FS_PATH_MAX], b[FS_PATH_MAX];
	int cursor = 0;
	while (fs_readdir(from, &cursor, name, &st) > 0) {
		join(a, from, name);
		join(b, to, name);
		r = copy_tree(a, b);
		if (r < 0)
			return r;
	}
	return FS_OK;
}

static void cmd_cp(int argc, char **argv)
{
	int recursive = has_flag(&argc, argv, "-r");
	if (argc != 3) {
		sh_error("Usage: cp [-r] <from> <to>");
		return;
	}
	char from[FS_PATH_MAX], to[FS_PATH_MAX];
	fs_resolve(argv[1], from);
	fs_resolve(argv[2], to);
	struct fs_stat st;
	if (fs_stat(to, &st) == FS_OK && st.dir) {
		char *base = strrchr(from, '/') + 1;
		char t[FS_PATH_MAX];
		join(t, to, base);
		strcpy(to, t);
	}
	if (fs_stat(from, &st) == FS_OK && st.dir && !recursive) {
		sh_error("cp: %s is a directory, use cp -r", argv[1]);
		return;
	}
	int r = recursive ? copy_tree(from, to) : copy_file(from, to);
	if (r < 0)
		sh_fs_error(argv[1], r);
}

static void cmd_mv(int argc, char **argv)
{
	if (argc != 3) {
		sh_error("Usage: mv <from> <to>");
		return;
	}
	int r = fs_rename(argv[1], argv[2]);
	if (r < 0)
		sh_fs_error(argv[1], r);
}

static void cmd_stat(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		struct fs_stat st;
		int r = fs_stat(argv[i], &st);
		if (r < 0) {
			sh_fs_error(argv[i], r);
			continue;
		}
		char mode[11], owner[32], date[32], abs[FS_PATH_MAX];
		mode_text(&st, mode);
		users_name_of(st.owner, owner, sizeof owner);
		fs_date(st.mtime, date, sizeof date);
		fs_resolve(argv[i], abs);
		out("  File: %s\n", abs);
		out("  Type: %s  Size: %u  Sectors: %u  Extents: %d\n", st.dir ? "directory" : "file", st.size,
		    st.sectors, st.extents);
		out("  Mode: %s (%03o)  Owner: %s  Entry: %d\n", mode, st.mode, owner, st.index);
		out("  Modified: %s\n", date);
	}
}

static void tree(const char *path, int depth, int *dirs, int *files)
{
	char name[FS_PATH_MAX], child[FS_PATH_MAX];
	struct fs_stat st;
	int cursor = 0;
	while (fs_readdir(path, &cursor, name, &st) > 0) {
		for (int i = 0; i < depth; i++)
			out("|   ");
		out("|-- ");
		out_color(st.dir ? LIGHT_BLUE : WHITE);
		out("%s\n", name);
		out_color(WHITE);
		if (st.dir) {
			(*dirs)++;
			join(child, path, name);
			if (depth < 16)
				tree(child, depth + 1, dirs, files);
		} else {
			(*files)++;
		}
	}
}

static void cmd_tree(int argc, char **argv)
{
	char abs[FS_PATH_MAX];
	fs_resolve(argc > 1 ? argv[1] : ".", abs);
	struct fs_stat st;
	int r = fs_stat(abs, &st);
	if (r < 0 || !st.dir) {
		sh_fs_error(argc > 1 ? argv[1] : ".", r < 0 ? r : FS_ENOTDIR);
		return;
	}
	int dirs = 0, files = 0;
	out_color(LIGHT_BLUE);
	out("%s\n", abs);
	out_color(WHITE);
	tree(abs, 0, &dirs, &files);
	out("\n%d directories, %d files\n", dirs, files);
}

static void dump(const uint8_t *p, int n)
{
	for (int at = 0; at < n; at += 16) {
		out("%08x  ", at);
		for (int i = 0; i < 16; i++)
			if (at + i < n)
				out("%02x ", p[at + i]);
			else
				out("   ");
		out(" ");
		for (int i = 0; i < 16 && at + i < n; i++)
			out("%c", p[at + i] >= 32 && p[at + i] < 127 ? p[at + i] : '.');
		out("\n");
	}
}

static void cmd_hexdump(int argc, char **argv)
{
	int n = sh_input(argc, argv, 1, data, sizeof data);
	if (n > 0)
		dump((uint8_t *)data, n);
}

static void cmd_fsinfo(int argc, char **argv)
{
	if (fs_device() < 0) {
		sh_error("No filesystem mounted");
		return;
	}
	uint32_t used, total;
	int files;
	fs_usage(&used, &total, &files);
	out("serpentis filesystem version 5 on %s\n", block_get(fs_device())->name);
	out("  entries  %d of %d\n", files, FS_MAX_FILES);
	out("  data     %u of %u sectors used, %u KB free\n", used, total, (total - used) / 2);
	out("  largest file %d KB\n", FS_FILE_MAX / 1024);
}

static void cmd_df(int argc, char **argv)
{
	uint32_t used, total;
	int files;
	if (fs_device() < 0) {
		sh_error("No filesystem mounted");
		return;
	}
	fs_usage(&used, &total, &files);
	out("Filesystem  Size    Used    Free    Use%%  Mounted on\n");
	out("%-10s  %5uK  %5uK  %5uK  %3u%%  /\n", block_get(fs_device())->name, total / 2, used / 2,
	    (total - used) / 2, total ? used * 100 / total : 0);
}

static uint32_t du(const char *path)
{
	struct fs_stat st;
	if (fs_stat(path, &st) < 0)
		return 0;
	if (!st.dir)
		return st.sectors;
	uint32_t total = 0;
	char name[FS_PATH_MAX], child[FS_PATH_MAX];
	int cursor = 0;
	while (fs_readdir(path, &cursor, name, &st) > 0) {
		join(child, path, name);
		total += du(child);
	}
	return total;
}

static void cmd_du(int argc, char **argv)
{
	char abs[FS_PATH_MAX];
	fs_resolve(argc > 1 ? argv[1] : ".", abs);
	out("%uK\t%s\n", du(abs) / 2, abs);
}

static void find(const char *path, const char *pattern)
{
	char name[FS_PATH_MAX], child[FS_PATH_MAX];
	struct fs_stat st;
	int cursor = 0;
	while (fs_readdir(path, &cursor, name, &st) > 0) {
		join(child, path, name);
		if (!pattern || strstr(name, pattern))
			out("%s\n", child);
		if (st.dir)
			find(child, pattern);
	}
}

static void cmd_find(int argc, char **argv)
{
	char abs[FS_PATH_MAX];
	const char *pattern = NULL;
	const char *start = ".";
	int plain = 0;
	const char *words[2];
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-name") && i + 1 < argc)
			pattern = argv[++i];
		else if (plain < 2)
			words[plain++] = argv[i];
	}
	if (plain == 2) {
		start = words[0];
		pattern = words[1];
	} else if (plain == 1) {
		struct fs_stat st;
		if (!pattern && fs_stat(words[0], &st) < 0)
			pattern = words[0];
		else
			start = words[0];
	}
	fs_resolve(start, abs);
	find(abs, pattern);
}

static void report(const char *msg)
{
	out("fsck: %s\n", msg);
}

static void cmd_fsck(int argc, char **argv)
{
	int fix = has_flag(&argc, argv, "-f");
	if (fs_device() < 0) {
		sh_error("No filesystem mounted");
		return;
	}
	int n = fs_check(fix, report);
	if (!n)
		out("fsck: clean\n");
	else
		out("fsck: %d problem%s%s\n", n, n == 1 ? "" : "s", fix ? ", fixed" : ", run fsck -f to fix");
}

// every write already went to the disk. here so scripts from serpentis-os dont break
static void cmd_sync(int argc, char **argv) {}

extern int sh_logout, sh_restart;

static void cmd_format(int argc, char **argv)
{
	if (!user_is_root()) {
		sh_error("format: Permission denied");
		return;
	}
	int dev = -1;
	if (argc == 2 && !strcmp(argv[1], "yes"))
		dev = fs_device();
	else if (argc == 3 && !strcmp(argv[2], "yes"))
		dev = block_find(argv[1]);
	else {
		sh_error("Usage: format yes   or   format <disk> yes   (see lsblk)");
		return;
	}
	if (dev < 0) {
		sh_error("format: no such disk");
		return;
	}
	int was_root = dev == fs_device();
	int r = fs_format(dev);
	if (r < 0) {
		sh_fs_error("format", r);
		return;
	}
	user_sudo(1);
	const char *dirs[] = {"/bin", "/etc", "/home", "/root", "/tmp", "/src"};
	for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++)
		fs_mkdir(dirs[i]);
	fs_chmod("/tmp", 0777);
	fs_chmod("/root", 0700);
	user_sudo(0);
	out("%s formatted, everything on it is gone\n", block_get(dev)->name);
	// that was our own disk, /etc/passwd went with it
	if (was_root) {
		out("no users left, setup starts again\n");
		sh_restart = 1;
	}
}

static void cmd_chmod(int argc, char **argv)
{
	long mode = 0;
	if (argc < 3) {
		sh_error("Usage: chmod <octal> <file>...");
		return;
	}
	for (const char *s = argv[1]; *s; s++) {
		if (*s < '0' || *s > '7') {
			sh_error("chmod: %s is not an octal mode", argv[1]);
			return;
		}
		mode = mode * 8 + *s - '0';
	}
	for (int i = 2; i < argc; i++) {
		int r = fs_chmod(argv[i], mode);
		if (r < 0)
			sh_fs_error(argv[i], r);
	}
}

static void cmd_chown(int argc, char **argv)
{
	struct user_info u;
	if (argc < 3) {
		sh_error("Usage: chown <user> <file>...");
		return;
	}
	if (users_find(argv[1], &u) < 0) {
		sh_error("chown: no user %s", argv[1]);
		return;
	}
	for (int i = 2; i < argc; i++) {
		int r = fs_chown(argv[i], u.uid);
		if (r < 0)
			sh_fs_error(argv[i], r);
	}
}

static void cmd_realpath(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		char abs[FS_PATH_MAX];
		if (fs_resolve(argv[i], abs) == FS_OK)
			out("%s\n", abs);
	}
}

static void cmd_basename(int argc, char **argv)
{
	if (argc < 2)
		return;
	char *s = argv[1];
	int n = strlen(s);
	while (n > 1 && s[n - 1] == '/')
		s[--n] = 0;
	char *slash = strrchr(s, '/');
	char *base = slash && slash[1] ? slash + 1 : s;
	if (argc > 2) {
		int bl = strlen(base), sl = strlen(argv[2]);
		if (sl < bl && !strcmp(base + bl - sl, argv[2]))
			base[bl - sl] = 0;
	}
	out("%s\n", base);
}

static void cmd_dirname(int argc, char **argv)
{
	if (argc < 2)
		return;
	char *s = argv[1];
	char *slash = strrchr(s, '/');
	if (!slash)
		out(".\n");
	else if (slash == s)
		out("/\n");
	else {
		*slash = 0;
		out("%s\n", s);
	}
}

// timebase bits as the random part
static void cmd_mktemp(int argc, char **argv)
{
	int dir = has_flag(&argc, argv, "-d");
	char path[64];
	for (int i = 0; i < 100; i++) {
		snprintf(path, sizeof path, "/tmp/tmp.%06lx", (long)((mftb() >> 4) + i) & 0xffffff);
		if (dir) {
			if (fs_mkdir(path) == FS_OK) {
				out("%s\n", path);
				return;
			}
			continue;
		}
		if (fs_touch(path) == FS_OK) {
			struct fs_stat st;
			if (fs_stat(path, &st) == FS_OK && st.size == 0) {
				out("%s\n", path);
				return;
			}
		}
	}
	sh_error("mktemp: no luck in /tmp");
}

static void cmd_truncate(int argc, char **argv)
{
	long size;
	if (argc < 4 || strcmp(argv[1], "-s") || sh_number(argv[2], &size) || size < 0 || size > FS_FILE_MAX) {
		sh_error("Usage: truncate -s <size> <file>...");
		return;
	}
	for (int i = 3; i < argc; i++) {
		int n = fs_read(argv[i], data, sizeof data);
		if (n < 0)
			n = 0;
		if (size > n)
			memset(data + n, 0, size - n);
		int r = fs_write(argv[i], data, size);
		if (r < 0)
			sh_fs_error(argv[i], r);
	}
}

static void cmd_split(int argc, char **argv)
{
	long lines = 1000;
	int first = 1;
	if (argc > 2 && !strcmp(argv[1], "-l")) {
		if (sh_number(argv[2], &lines) || lines < 1) {
			sh_error("Usage: split [-l lines] <file> [prefix]");
			return;
		}
		first = 3;
	}
	if (first >= argc) {
		sh_error("Usage: split [-l lines] <file> [prefix]");
		return;
	}
	const char *prefix = first + 1 < argc ? argv[first + 1] : "x";
	int n = fs_read(argv[first], data, sizeof data);
	if (n < 0) {
		sh_fs_error(argv[first], n);
		return;
	}
	int part = 0, start = 0, count = 0;
	for (int i = 0; i <= n; i++) {
		if (i < n && data[i] == '\n')
			count++;
		if ((count == lines && i < n) || (i == n && i > start)) {
			char name[FS_PATH_MAX];
			snprintf(name, sizeof name, "%s%c%c", prefix, 'a' + part / 26, 'a' + part % 26);
			int end = i < n ? i + 1 : i;
			fs_write(name, data + start, end - start);
			part++;
			start = end;
			count = 0;
		}
	}
}

static void cmd_dd(int argc, char **argv)
{
	const char *in = NULL, *outp = NULL;
	long bs = 512, count = -1, skip = 0;
	for (int i = 1; i < argc; i++) {
		if (!strncmp(argv[i], "if=", 3))
			in = argv[i] + 3;
		else if (!strncmp(argv[i], "of=", 3))
			outp = argv[i] + 3;
		else if (!strncmp(argv[i], "bs=", 3))
			sh_number(argv[i] + 3, &bs);
		else if (!strncmp(argv[i], "count=", 6))
			sh_number(argv[i] + 6, &count);
		else if (!strncmp(argv[i], "skip=", 5))
			sh_number(argv[i] + 5, &skip);
	}
	if (!in || bs < 1) {
		sh_error("Usage: dd if=<file|disk> [of=<file>] [bs=512] [count=n] [skip=n]");
		return;
	}
	int len;
	// if= can be a disk from lsblk, then its raw sectors. handy for looking at an mbr
	int disk = block_find(in);
	if (disk >= 0) {
		if (count < 0)
			count = 1;
		if (bs % 512 || bs * count > (long)sizeof data) {
			sh_error("dd: on a disk bs has to be whole sectors and it all has to fit in 1 MB");
			return;
		}
		if (block_read(disk, skip * bs / 512, bs * count / 512, data) < 0) {
			sh_error("dd: read error");
			return;
		}
		len = bs * count;
	} else {
		len = fs_read(in, data, sizeof data);
		if (len < 0) {
			sh_fs_error(in, len);
			return;
		}
		long from = skip * bs;
		long take = count < 0 ? len - from : count * bs;
		if (from > len)
			from = len;
		if (take > len - from)
			take = len - from;
		memmove(data, data + from, take);
		len = take;
	}
	if (outp) {
		int r = fs_write(outp, data, len);
		if (r < 0)
			sh_fs_error(outp, r);
	} else {
		out_write(data, len);
	}
	out("%d bytes copied\n", len);
}

static void cmd_file(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		struct fs_stat st;
		if (fs_stat(argv[i], &st) < 0) {
			sh_error("%s: cannot open", argv[i]);
			continue;
		}
		if (st.dir) {
			out("%s: directory\n", argv[i]);
			continue;
		}
		int n = fs_read(argv[i], data, sizeof data);
		const char *kind = "data";
		if (n == 0)
			kind = "empty";
		else if (n >= 4 && !memcmp(data, "SERP", 4))	// a serpentis-os program, from a disk that was there
			kind = "serpentis executable";
		else if (n >= 4 && !memcmp(data, "\177ELF", 4))
			kind = "ELF executable";
		else {
			int text = 1;
			for (int k = 0; k < n && text; k++)
				text = (data[k] >= 32 && data[k] < 127) || data[k] == '\n' || data[k] == '\t' || data[k] == '\r';
			if (text)
				kind = "ASCII text";
		}
		out("%s: %s\n", argv[i], kind);
	}
}

// no /bin here, every command lives in the kernel
static void cmd_which(int argc, char **argv)
{
	for (int i = 1; i < argc; i++)
		out("%s: built into the shell\n", argv[i]);
}

const struct command cmd_fs[] = {
	{"ls", cmd_ls}, {"cd", cmd_cd}, {"mkdir", cmd_mkdir}, {"rmdir", cmd_rmdir}, {"rm", cmd_rm},
	{"touch", cmd_touch}, {"write", cmd_write}, {"append", cmd_append}, {"cat", cmd_cat},
	{"cp", cmd_cp}, {"mv", cmd_mv}, {"stat", cmd_stat}, {"tree", cmd_tree}, {"hexdump", cmd_hexdump},
	{"fsinfo", cmd_fsinfo}, {"df", cmd_df}, {"du", cmd_du}, {"find", cmd_find}, {"fsck", cmd_fsck},
	{"sync", cmd_sync}, {"format", cmd_format}, {"chmod", cmd_chmod}, {"chown", cmd_chown},
	{"realpath", cmd_realpath}, {"basename", cmd_basename}, {"dirname", cmd_dirname},
	{"mktemp", cmd_mktemp}, {"truncate", cmd_truncate}, {"split", cmd_split}, {"dd", cmd_dd},
	{"file", cmd_file}, {"which", cmd_which},
	{NULL, NULL}
};
