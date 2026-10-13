#include "kernel.h"
#include "ppc.h"
#include "screen.h"
#include "shell.h"

void sh_history(void);

// by hand, nothing checks that it matches the tables
static const char help_text[] =
	"SERPENTIS commands:\n"
	" help clear echo version about banner logo pwd\n"
	" whoami hostname uname history strlen repeat\n"
	" ticks uptime cpuinfo cpuid date time irqinfo\n"
	" palette rand hex dec beep\n"
	" ls cd mkdir mkdir -p rmdir rmdir -r\n"
	" touch write append cat rm rm -r rm -f\n"
	" cp mv stat tree hexdump wc fsinfo sync format\n"
	" find du df fsck\n"
	" ps mem lsusb lsblk mouse mousepos\n"
	" games: pacman\n"
	" programs: edit vi src\n"
	" keys: arrows go anywhere, enter runs the line under the cursor,\n"
	"       ctrl+up/down history. in edit: ctrl+s save, ctrl+q quit\n"
	" text: head tail grep sort uniq cut rev tac nl cmp diff less more\n"
	"       tr tee wc paste comm fold strings xxd cksum xargs\n"
	" files: split truncate dd realpath mktemp basename dirname which file\n"
	" utils: seq sleep true false\n"
	" users: id su passwd useradd chmod chown logout, ls -l shows owners\n"
	" reboot shutdown halt\n";

static const char about_text[] =
	"serpentis360, a kernel for the xbox 360 written from zero.\n"
	"same filesystem and users as serpentis-os, so a disk moves between them.\n";

static const char banner_text[] =
	"serpentis-os\n"
	"The serpent is awake.\n";

static const char snake_logo_text[] =
	"              __\n"
	"     ________/  \\___\n"
	"  **/               \\**\n"
	" <__   SERPENTIS      _>\n"
	"    \\___/\\___/\\______/\n";

static void args(int argc, char **argv, int from)
{
	for (int i = from; i < argc; i++)
		out(i > from ? " %s" : "%s", argv[i]);
	out("\n");
}

static void cmd_help(int argc, char **argv) { out("%s", help_text); }
static void cmd_clear(int argc, char **argv) { terminal_clear(); }
static void cmd_version(int argc, char **argv) { out("serpentis-os ppc64\n"); }
static void cmd_about(int argc, char **argv) { out("%s", about_text); }
static void cmd_pwd(int argc, char **argv) { out("%s\n", fs_cwd()); }
static void cmd_whoami(int argc, char **argv) { out("%s\n", users_current()->name); }
static void cmd_hostname(int argc, char **argv) { out("serpentis\n"); }
static void cmd_uname(int argc, char **argv) { out("serpentis-os ppc64 %s\n", platform_name()); }
static void cmd_history(int argc, char **argv) { sh_history(); }

static void cmd_echo(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "-n")) {
		for (int i = 2; i < argc; i++)
			out(i > 2 ? " %s" : "%s", argv[i]);
		return;
	}
	args(argc, argv, 1);
}

static void cmd_banner(int argc, char **argv)
{
	out_color(LIGHT_GREEN);
	out("%s", banner_text);
	out_color(WHITE);
}

static void cmd_logo(int argc, char **argv)
{
	out_color(LIGHT_GREEN);
	out("%s", snake_logo_text);
	out_color(WHITE);
}

static void cmd_strlen(int argc, char **argv)
{
	if (argc < 2) {
		sh_error("Usage: strlen <text>");
		return;
	}
	long n = argc - 2;
	for (int i = 1; i < argc; i++)
		n += strlen(argv[i]);
	out("Length: %ld\n", n);
}

static void cmd_repeat(int argc, char **argv)
{
	long n;
	if (argc < 3 || sh_number(argv[1], &n) || n < 1 || n > 50) {
		sh_error("Usage: repeat <1-50> <text>");
		return;
	}
	while (n--)
		args(argc, argv, 2);
}

const struct command cmd_core[] = {
	{"help", cmd_help}, {"clear", cmd_clear}, {"echo", cmd_echo}, {"version", cmd_version},
	{"about", cmd_about}, {"banner", cmd_banner}, {"logo", cmd_logo}, {"pwd", cmd_pwd},
	{"whoami", cmd_whoami}, {"hostname", cmd_hostname}, {"uname", cmd_uname},
	{"history", cmd_history}, {"strlen", cmd_strlen}, {"repeat", cmd_repeat},
	{NULL, NULL}
};
