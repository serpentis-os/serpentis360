#include "kernel.h"
#include "ppc.h"
#include "screen.h"
#include "shell.h"

extern int sh_logout, sh_restart;
void shell_session(void);

static void cmd_id(int argc, char **argv)
{
	struct user_info u = *users_current();
	if (argc > 1 && users_find(argv[1], &u) < 0) {
		sh_error("id: no user %s", argv[1]);
		return;
	}
	out("uid=%d(%s)\n", u.uid, u.name);
}

// a whole new session on top of this one. root does not get asked
static void cmd_su(int argc, char **argv)
{
	const char *name = argc > 1 ? argv[1] : "root";
	struct user_info u, back = *users_current();
	if (users_find(name, &u) < 0) {
		sh_error("su: no user %s", name);
		return;
	}
	if (back.uid != 0) {
		char pw[64];
		sh_read_line("Password: ", pw, sizeof pw, 1);
		int ok = users_check(name, pw) == 0;
		memset(pw, 0, sizeof pw);
		if (!ok) {
			sh_error("su: Authentication failure");
			return;
		}
	}
	char cwd[FS_PATH_MAX];
	strcpy(cwd, fs_cwd());
	users_login(&u);
	shell_session();
	users_login(&back);
	if (fs_chdir(cwd) < 0)
		fs_chdir(back.home);
	sh_logout = sh_restart;
}

// root changes anyone, everyone else only themselves and only with the old password
static void cmd_passwd(int argc, char **argv)
{
	const struct user_info *me = users_current();
	const char *name = argc > 1 ? argv[1] : me->name;
	struct user_info u;
	if (users_find(name, &u) < 0) {
		sh_error("passwd: no user %s", name);
		return;
	}
	if (me->uid != 0 && u.uid != me->uid) {
		sh_error("passwd: Permission denied");
		return;
	}
	char pw[64], again[64];
	if (me->uid != 0) {
		sh_read_line("Current password: ", pw, sizeof pw, 1);
		int ok = users_check(name, pw) == 0;
		if (!ok) {
			sh_error("passwd: Authentication failure");
			return;
		}
	}
	sh_read_line("New password: ", pw, sizeof pw, 1);
	sh_read_line("Retype new password: ", again, sizeof again, 1);
	if (strcmp(pw, again)) {
		sh_error("passwd: they do not match");
		return;
	}
	int r = users_set_password(name, pw);
	memset(pw, 0, sizeof pw);
	memset(again, 0, sizeof again);
	if (r < 0)
		sh_fs_error("passwd", r);
	else
		out("passwd: password updated\n");
}

static void cmd_useradd(int argc, char **argv)
{
	if (!user_is_root()) {
		sh_error("useradd: Permission denied");
		return;
	}
	if (argc != 2) {
		sh_error("Usage: useradd <name>");
		return;
	}
	if (users_fix_name(argv[1]) < 0) {
		sh_error("useradd: no spaces, : or / in a name");
		return;
	}
	int uid = 1000;
	struct user_info u;
	char name[32];
	while (users_name_of(uid, name, sizeof name) == 0)
		uid++;
	if (users_find(argv[1], &u) == 0) {
		sh_error("useradd: %s exists", argv[1]);
		return;
	}
	char pw[64], again[64];
	sh_read_line("Password: ", pw, sizeof pw, 1);
	sh_read_line("Again: ", again, sizeof again, 1);
	if (strcmp(pw, again)) {
		sh_error("useradd: they do not match");
		return;
	}
	int r = users_add(argv[1], pw, uid);
	memset(pw, 0, sizeof pw);
	if (r < 0)
		sh_fs_error("useradd", r);
	else
		out("added %s, uid %d, home /home/%s\n", argv[1], uid, argv[1]);
}

static void cmd_logout(int argc, char **argv)
{
	sh_logout = 1;
}

static void cmd_edit(int argc, char **argv)
{
	editor_main(argc > 1 ? argv[1] : NULL);
}

const struct command cmd_user[] = {
	{"id", cmd_id}, {"su", cmd_su}, {"passwd", cmd_passwd}, {"useradd", cmd_useradd},
	{"logout", cmd_logout}, {"exit", cmd_logout}, {"edit", cmd_edit}, {"vi", cmd_edit},
	{NULL, NULL}
};
