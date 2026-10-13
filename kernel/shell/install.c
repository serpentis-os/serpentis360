#include "kernel.h"
#include "ppc.h"
#include "screen.h"
#include "block.h"
#include "usb.h"
#include "shell.h"

// no serpentis disk found = first boot, ask where to live and who you are

#define PART_TYPE 0x53		// 'S', nobody else uses it
#define MIN_SECTORS FS_DISK_SECTORS

extern int usb_settled;
extern int sh_logout, sh_restart;
void shell_session(void);

static void say(int color, const char *text)
{
	terminal_color(color);
	out("%s", text);
	terminal_color(WHITE);
}

static int mount_any(void)
{
	for (int pass = 0; pass < 2; pass++)
		for (int i = 0; i < BLOCK_MAX; i++) {
			struct block *b = block_get(i);
			if (!b || (pass == 0) != (b->parent >= 0))
				continue;
			if (fs_mount(i) == FS_OK) {
				klog("fs: mounted %s on /\n", b->name);
				return i;
			}
		}
	return -1;
}

int install_needed(void)
{
	return fs_device() < 0 && mount_any() < 0;
}

struct choice {
	int dev;
	int disk;
	uint64_t start;
};

static int ask_yes(const char *question)
{
	char answer[16];
	out("%s [yes/no] ", question);
	sh_read_line(NULL, answer, sizeof answer, 0);
	return !strcmp(answer, "yes") || !strcmp(answer, "y");
}

static int ask_password(const char *who, char *out_pw, int room)
{
	char again[64];
	for (;;) {
		char q[80];
		snprintf(q, sizeof q, "%s password: ", who);
		sh_read_line(q, out_pw, room, 1);
		snprintf(q, sizeof q, "%s password again: ", who);
		sh_read_line(q, again, sizeof again, 1);
		if (!strcmp(out_pw, again))
			return 0;
		say(LIGHT_RED, "they do not match, again\n");
	}
}

// every partition that is big enough and the free tail of every disk. the fat32 one xell
// boots from is in the list too, picking it means copying xenon.elf again
static int pick_disk(void)
{
	struct choice choices[BLOCK_MAX * 2];
	int n = 0;
	say(LIGHT_CYAN, "\nwhere should serpentis-os live?\n\n");
	for (int i = 0; i < BLOCK_MAX; i++) {
		struct block *b = block_get(i);
		if (!b)
			continue;
		uint64_t mb = b->sectors / 2048;
		if (b->parent >= 0 && b->sectors >= MIN_SECTORS) {
			out("  %d  partition %-8s %5ld MB, type %02x  (erased)\n", n + 1, b->name, (long)mb, b->type);
			choices[n++] = (struct choice){i, -1, 0};
		} else if (b->parent < 0) {
			uint64_t start, room = block_free_tail(i, &start);
			if (room >= MIN_SECTORS) {
				out("  %d  new partition on %-5s %5ld MB free at the end, the rest stays as it is\n", n + 1,
				    b->name, (long)(room / 2048));
				choices[n++] = (struct choice){-1, i, start};
			}
		}
	}
	out("  %d  memory only, gone at power off\n", n + 1);
	choices[n++] = (struct choice){-2, -1, 0};

	for (;;) {
		char answer[16];
		long pick;
		out("\nnumber: ");
		sh_read_line(NULL, answer, sizeof answer, 0);
		if (sh_number(answer, &pick) || pick < 1 || pick > n)
			continue;
		struct choice c = choices[pick - 1];
		if (c.dev == -2) {
			int ram = block_find("ram0");
			if (ram < 0)
				ram = ramdisk_create(FS_DISK_SECTORS);
			return ram;
		}
		if (c.dev >= 0) {
			char q[96];
			snprintf(q, sizeof q, "everything on %s gets erased, sure?", block_get(c.dev)->name);
			if (!ask_yes(q))
				continue;
			return c.dev;
		}
		int slot = 0;
		for (int s = 1; s <= 4 && !slot; s++) {
			int taken = 0;
			for (int i = 0; i < BLOCK_MAX; i++) {
				struct block *b = block_get(i);
				taken |= b && b->parent == c.disk && b->part == s;
			}
			if (!taken)
				slot = s;
		}
		if (!slot) {
			say(LIGHT_RED, "all four partition slots are taken on that disk\n");
			continue;
		}
		int p = block_add_partition(c.disk, slot, PART_TYPE, c.start, MIN_SECTORS);
		if (p < 0) {
			say(LIGHT_RED, "could not write the partition table\n");
			continue;
		}
		out("made %s, 64 MB\n", block_get(p)->name);
		return p;
	}
}

static void setup_users(void)
{
	char name[32], password[64], root[64];
	say(LIGHT_CYAN, "\nusers\n");
	for (;;) {
		while (input_getc() >= 0)
			;
		sh_read_line("your user name: ", name, sizeof name, 0);
		int bad = users_fix_name(name);
		if (!name[0])
			continue;
		if (!bad && strcmp(name, "root"))
			break;
		terminal_color(LIGHT_RED);
		out("can't use \"");
		for (char *s = name; *s; s++)
			out(*s >= ' ' && *s <= '~' ? "%c" : "\\x%02x", *s & 0xff);
		out("\", %d chars. no spaces, : or / and not root\n", (int)strlen(name));
		terminal_color(WHITE);
	}
	ask_password(name, password, sizeof password);
	ask_password("root", root, sizeof root);
	users_add("root", root, 0);
	users_add(name, password, 1000);
	memset(password, 0, sizeof password);
	memset(root, 0, sizeof root);
}

int install_run(void)
{
	say(LIGHT_GREEN, "\nserpentis-os setup\n");
	out("nothing to boot from yet, so this sets it up once.\n");
	int dev = pick_disk();
	if (dev < 0) {
		say(LIGHT_RED, "no disk to use\n");
		return -1;
	}
	out("formatting %s ...\n", block_get(dev)->name);
	int r = fs_format(dev);
	if (r < 0) {
		sh_fs_error("format", r);
		return -1;
	}
	user_sudo(1);
	const char *dirs[] = {"/bin", "/etc", "/home", "/root", "/tmp", "/src"};
	for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++)
		fs_mkdir(dirs[i]);
	fs_chmod("/tmp", 0777);
	fs_chmod("/root", 0700);
	fs_write("/etc/hostname", "serpentis\n", 10);
	user_sudo(0);
	out("putting the source in /src/serpentis360 ...\n");
	source_unpack("/src/serpentis360");
	setup_users();
	say(LIGHT_GREEN, "\ndone. now log in\n");
	return 0;
}

static void banner(void)
{
	terminal_clear();
	say(LIGHT_CYAN, "\n  serpentis-os   who are you\n\n");
}

// the whole life of the shell thread. setup if needed, login, session, again
void login_run(void)
{
	while (!usb_settled)
		sleep_ms(50);
	for (;;) {
		sh_restart = 0;
		if (install_needed() && install_run() < 0) {
			sleep_ms(2000);
			continue;
		}
		if (!users_exist()) {
			setup_users();
			say(LIGHT_GREEN, "\ndone. now log in\n");
			sleep_ms(1000);
			continue;
		}
		source_check();
		banner();
		char name[32], password[64];
		struct user_info u;
		for (;;) {
			if (sh_read_line("  user      ", name, sizeof name, 0) < 0)
				continue;
			sh_read_line("  password  ", password, sizeof password, 1);
			users_fix_name(name);
			int ok = users_find(name, &u) == 0 && users_check(name, password) == 0;
			memset(password, 0, sizeof password);
			if (ok)
				break;
			say(LIGHT_RED, "  wrong user or password\n\n");
		}
		users_login(&u);
		if (fs_chdir(u.home) < 0)
			fs_chdir("/");
		terminal_clear();
		shell_session();
		struct user_info none = {"nobody", 65534, "/"};
		users_login(&none);
	}
}
