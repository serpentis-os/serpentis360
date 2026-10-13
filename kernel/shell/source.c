#include "kernel.h"
#include "screen.h"
#include "shell.h"

// the tree this kernel was built from, as a ustar inside it

#define SRC_DIR "/src/serpentis360"

extern const char source_tar[], source_tar_end[];

static long octal(const char *s, int n)
{
	long v = 0;
	for (; n > 0 && *s >= '0' && *s <= '7'; s++, n--)
		v = v * 8 + *s - '0';
	return v;
}

static int walk(int (*fn)(const char *name, char type, const char *data, long size, void *arg), void *arg)
{
	const char *at = source_tar;
	int n = 0;
	while (at + 512 <= source_tar_end && at[0]) {
		char name[256];
		int len = 0;
		if (at[345]) {
			for (int i = 0; i < 155 && at[345 + i]; i++)
				name[len++] = at[345 + i];
			name[len++] = '/';
		}
		for (int i = 0; i < 100 && at[i]; i++)
			name[len++] = at[i];
		name[len] = 0;
		long size = octal(at + 124, 12);
		while (len && name[len - 1] == '/')
			name[--len] = 0;
		if (fn(name, at[156], at + 512, size, arg))
			break;
		n++;
		at += 512 + (size + 511) / 512 * 512;
	}
	return n;
}

static int list_one(const char *name, char type, const char *data, long size, void *arg)
{
	if (type == '5')
		return 0;
	out("%7ld  %s\n", size, name);
	*(long *)arg += size;
	return 0;
}

struct unpack {
	const char *to;
	int owner, files, errors;
};

// /src belongs to root, a copy into your home is yours
static int unpack_one(const char *name, char type, const char *data, long size, void *arg)
{
	struct unpack *u = arg;
	char path[FS_PATH_MAX];
	snprintf(path, sizeof path, "%s/%s", u->to, name);
	if (type == '5') {
		struct fs_stat st;
		if (fs_stat(path, &st) < 0 && fs_mkdir(path) < 0)
			u->errors++;
		fs_chown(path, u->owner);
		return 0;
	}
	if (fs_write(path, data, size) < 0) {
		u->errors++;
		return 0;
	}
	fs_chown(path, u->owner);
	u->files++;
	return 0;
}

static void mkdir_p(const char *p)
{
	char part[FS_PATH_MAX];
	struct fs_stat st;
	for (int i = 1; p[i - 1] && i < FS_PATH_MAX; i++)
		if (p[i] == '/' || !p[i]) {
			memcpy(part, p, i);
			part[i] = 0;
			if (fs_stat(part, &st) < 0)
				fs_mkdir(part);
		}
}

int source_unpack(const char *to)
{
	struct unpack u = {to, strcmp(to, SRC_DIR) ? user_uid() : 0, 0, 0};
	user_sudo(1);
	mkdir_p(to);
	walk(unpack_one, &u);
	user_sudo(0);
	return u.errors ? -u.errors : u.files;
}

void source_check(void)
{
	struct fs_stat st;
	if (fs_device() >= 0 && fs_stat(SRC_DIR, &st) < 0)
		source_unpack(SRC_DIR);
}

static void cmd_src(int argc, char **argv)
{
	if (argc == 1) {
		long total = 0;
		int n = walk(list_one, &total);
		out("%d entries, %ld bytes. src unpack [dir] writes them out, %s by default\n", n, total, SRC_DIR);
		return;
	}
	if (!strcmp(argv[1], "unpack")) {
		char to[FS_PATH_MAX];
		fs_resolve(argc > 2 ? argv[2] : SRC_DIR, to);
		if (strcmp(to, SRC_DIR) && !user_is_root() && strncmp(to, users_current()->home, strlen(users_current()->home))) {
			sh_error("src: only root unpacks outside your home");
			return;
		}
		int r = source_unpack(to);
		if (r < 0)
			sh_error("src: %d files could not be written", -r);
		else
			out("%d files in %s\n", r, to);
		return;
	}
	sh_error("Usage: src [unpack [dir]]");
}

const struct command cmd_source[] = {
	{"src", cmd_src},
	{NULL, NULL}
};
