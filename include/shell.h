#ifndef SHELL_H
#define SHELL_H

#include <stdarg.h>
#include <stdint.h>
#include "fs.h"

#define SH_LINE 512
#define SH_ARGS 32
#define SH_BUF (256 * 1024)	// what one stage of a pipe can hand to the next

typedef void (*command_fn)(int argc, char **argv);

struct command {
	const char *name;
	command_fn fn;
};

void out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void out_write(const char *s, int len);
void out_color(int color);
void sh_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void sh_fs_error(const char *what, int code);
int sh_stdin(const char **data);
int sh_input(int argc, char **argv, int first, char *buf, int room);
int sh_read_key(void);
int sh_read_line(const char *prompt, char *buf, int room, int hidden);
int sh_number(const char *s, long *v);
int sh_run(char *line);

// one table per file, the shell walks them in this order
extern const struct command cmd_core[], cmd_fs[], cmd_text[], cmd_user[], cmd_sys[], cmd_source[], cmd_games[];
int source_unpack(const char *to);
void source_check(void);

struct user_info {
	char name[32];
	int uid;
	char home[64];
};
int users_find(const char *name, struct user_info *u);
int users_name_of(int uid, char *name, int room);
int users_check(const char *name, const char *password);
int users_set_password(const char *name, const char *password);
int users_add(const char *name, const char *password, int uid);
int users_fix_name(char *name);
void users_login(const struct user_info *u);
const struct user_info *users_current(void);
int users_exist(void);

int editor_main(const char *path);

int install_needed(void);
int install_run(void);
void login_run(void);

#endif
