#include "kernel.h"
#include "ppc.h"
#include "shell.h"

// passwd and shadow exactly like serpentis-os, a disk from the x86 box logs in here

#define SALT_BYTES 8
#define TEXT_MAX 4096

void sha256(const void *data, size_t len, uint8_t out[32]);

static struct user_info current = {"root", 0, "/root"};
static int sudo;
static char text[TEXT_MAX];

int user_uid(void)
{
	return current.uid;
}

int user_is_root(void)
{
	return current.uid == 0 || sudo;
}

void user_sudo(int on)
{
	sudo = on;
}

void users_login(const struct user_info *u)
{
	current = *u;
}

const struct user_info *users_current(void)
{
	return &current;
}

static int load(const char *path)
{
	user_sudo(1);
	int n = fs_read(path, text, TEXT_MAX - 1);
	user_sudo(0);
	if (n < 0)
		n = 0;
	text[n] = 0;
	return n;
}

// field k of a name:x:y line
static int field(const char *line, int k, char *out, int room)
{
	while (k-- > 0) {
		line = strchr(line, ':');
		if (!line)
			return -1;
		line++;
	}
	int n = 0;
	while (line[n] && line[n] != ':' && line[n] != '\n' && n < room - 1) {
		out[n] = line[n];
		n++;
	}
	out[n] = 0;
	return n;
}

static const char *next_line(const char *p)
{
	p = strchr(p, '\n');
	return p ? p + 1 : NULL;
}

int users_exist(void)
{
	struct fs_stat st;
	return fs_stat("/etc/passwd", &st) == FS_OK;
}

int users_find(const char *name, struct user_info *u)
{
	load("/etc/passwd");
	for (const char *l = text; l && *l; l = next_line(l)) {
		char n[32], uid[16];
		if (field(l, 0, n, sizeof n) < 0 || strcmp(n, name))
			continue;
		field(l, 1, uid, sizeof uid);
		strcpy(u->name, n);
		long v = 0;
		sh_number(uid, &v);
		u->uid = v;
		field(l, 2, u->home, sizeof u->home);
		return 0;
	}
	return -1;
}

int users_name_of(int uid, char *name, int room)
{
	load("/etc/passwd");
	for (const char *l = text; l && *l; l = next_line(l)) {
		char n[32], u[16];
		long v = -1;
		field(l, 0, n, sizeof n);
		field(l, 1, u, sizeof u);
		if (!sh_number(u, &v) && v == uid) {
			snprintf(name, room, "%s", n);
			return 0;
		}
	}
	snprintf(name, room, "%d", uid);
	return -1;
}

static void hex(const uint8_t *in, int n, char *out)
{
	static const char digits[] = "0123456789abcdef";
	for (int i = 0; i < n; i++) {
		out[i * 2] = digits[in[i] >> 4];
		out[i * 2 + 1] = digits[in[i] & 15];
	}
	out[n * 2] = 0;
}

static int unhex(const char *s, uint8_t *out, int n)
{
	for (int i = 0; i < n; i++) {
		int v = 0;
		for (int k = 0; k < 2; k++) {
			char c = s[i * 2 + k];
			v <<= 4;
			if (c >= '0' && c <= '9')
				v |= c - '0';
			else if (c >= 'a' && c <= 'f')
				v |= c - 'a' + 10;
			else
				return -1;
		}
		out[i] = v;
	}
	return 0;
}

static void hash(const uint8_t salt[SALT_BYTES], const char *password, char out[65])
{
	uint8_t input[SALT_BYTES + 128], digest[32];
	int n = strlen(password);
	if (n > 128)
		n = 128;
	memcpy(input, salt, SALT_BYTES);
	memcpy(input + SALT_BYTES, password, n);
	sha256(input, SALT_BYTES + n, digest);
	hex(digest, 32, out);
}

int users_check(const char *name, const char *password)
{
	load("/etc/shadow");
	for (const char *l = text; l && *l; l = next_line(l)) {
		char n[32], salt_hex[40], want[80], got[65];
		uint8_t salt[SALT_BYTES];
		field(l, 0, n, sizeof n);
		if (strcmp(n, name))
			continue;
		if (field(l, 1, salt_hex, sizeof salt_hex) != SALT_BYTES * 2 || unhex(salt_hex, salt, SALT_BYTES))
			return -1;
		field(l, 2, want, sizeof want);
		hash(salt, password, got);
		return strcmp(got, want) ? -1 : 0;
	}
	return -1;
}

// shadow comes back without the old line and the new one goes at the end. the salt is
// timebase bits through xorshift, it only has to be different every time
int users_set_password(const char *name, const char *password)
{
	static char outbuf[TEXT_MAX];
	load("/etc/shadow");
	int len = 0;
	for (const char *l = text; l && *l; l = next_line(l)) {
		char n[32];
		field(l, 0, n, sizeof n);
		if (!strcmp(n, name))
			continue;
		const char *end = strchr(l, '\n');
		int ll = end ? end - l + 1 : (int)strlen(l);
		if (len + ll >= TEXT_MAX)
			return -1;
		memcpy(outbuf + len, l, ll);
		len += ll;
	}
	uint8_t salt[SALT_BYTES];
	uint64_t t = mftb() ^ ticks * 0x9e3779b97f4a7c15ull;
	for (int i = 0; i < SALT_BYTES; i++) {
		t ^= t << 13;
		t ^= t >> 7;
		t ^= t << 17;
		salt[i] = t >> 24;
	}
	char salt_hex[SALT_BYTES * 2 + 1], digest[65];
	hex(salt, SALT_BYTES, salt_hex);
	hash(salt, password, digest);
	len += snprintf(outbuf + len, TEXT_MAX - len, "%s:%s:%s\n", name, salt_hex, digest);
	user_sudo(1);
	int r = fs_write("/etc/shadow", outbuf, len);
	if (r == FS_OK)
		r = fs_chmod("/etc/shadow", 0600);
	user_sudo(0);
	return r;
}

// only : and / can break passwd or the home path, the rest is fine. spaces around go
int users_fix_name(char *name)
{
	char *s = name;
	while (*s == ' ' || *s == '\t')
		s++;
	memmove(name, s, strlen(s) + 1);
	int n = strlen(name);
	while (n && (name[n - 1] == ' ' || name[n - 1] == '\t'))
		name[--n] = 0;
	if (!n || n > 31)
		return -1;
	for (s = name; *s; s++) {
		if (*s >= 'A' && *s <= 'Z')
			*s += 32;
		if (*s == ':' || *s == '/' || *s == ' ' || *s < ' ' || *s > '~')
			return -1;
	}
	return 0;
}

int users_add(const char *name, const char *password, int uid)
{
	struct user_info u;
	if (users_find(name, &u) == 0)
		return FS_EEXIST;
	char line[128], home[64];
	if (uid == 0)
		strcpy(home, "/root");
	else
		snprintf(home, sizeof home, "/home/%s", name);
	snprintf(line, sizeof line, "%s:%d:%s\n", name, uid, home);
	user_sudo(1);
	int r = fs_append("/etc/passwd", line, strlen(line));
	if (r == FS_OK) {
		fs_mkdir(home);
		fs_chown(home, uid);
		fs_chmod(home, uid ? 0755 : 0700);
	}
	user_sudo(0);
	if (r == FS_OK)
		r = users_set_password(name, password);
	return r;
}
