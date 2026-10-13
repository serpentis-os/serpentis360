#include "kernel.h"
#include "ppc.h"
#include "screen.h"
#include "usb.h"
#include "shell.h"
#include "keys.h"

// no processes yet, so a pipe is a buffer between two stages

#define HISTORY 32

static char *capture;
static int capture_len;
static const char *stdin_buf;
static int stdin_len;
static char stage_a[SH_BUF], stage_b[SH_BUF];
static char history[HISTORY][SH_LINE];
static int history_count;

static void console_write(const char *s, int n)
{
	uint64_t msr = irq_off();
	for (int i = 0; i < n; i++) {
		if (s[i] == '\n')
			platform_putc('\r');
		platform_putc(s[i]);
		terminal_putc(s[i]);
	}
	irq_restore(msr);
}

// the screen, or the stage buffer when a pipe or a > is waiting for the output
void out_write(const char *s, int n)
{
	if (!capture) {
		console_write(s, n);
		return;
	}
	if (capture_len + n > SH_BUF)
		n = SH_BUF - capture_len;
	memcpy(capture + capture_len, s, n);
	capture_len += n;
}

void out(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	out_write(buf, n < (int)sizeof buf ? n : (int)sizeof buf - 1);
}

void out_color(int color)
{
	if (!capture)
		terminal_color(color);
}

void sh_error(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	terminal_color(LIGHT_RED);
	console_write(buf, strlen(buf));
	console_write("\n", 1);
	terminal_color(WHITE);
}

void sh_fs_error(const char *what, int code)
{
	sh_error("%s: %s", what, fs_error(code));
}

int sh_stdin(const char **data)
{
	*data = stdin_buf;
	return stdin_buf ? stdin_len : -1;
}

// files named from argv[first] on, or the pipe when there are none
int sh_input(int argc, char **argv, int first, char *buf, int room)
{
	if (first >= argc) {
		if (!stdin_buf) {
			sh_error("%s: no input, give a file or a pipe", argv[0]);
			return -1;
		}
		int n = stdin_len < room ? stdin_len : room;
		memcpy(buf, stdin_buf, n);
		return n;
	}
	int len = 0;
	for (int i = first; i < argc; i++) {
		int n = fs_read(argv[i], buf + len, room - len);
		if (n < 0) {
			sh_fs_error(argv[i], n);
			return -1;
		}
		len += n;
	}
	return len;
}

int sh_number(const char *s, long *v)
{
	int neg = 0, base = 10;
	long r = 0;
	if (*s == '-') {
		neg = 1;
		s++;
	}
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		base = 16;
		s += 2;
	}
	if (!*s)
		return -1;
	for (; *s; s++) {
		int d;
		if (*s >= '0' && *s <= '9')
			d = *s - '0';
		else if (base == 16 && *s >= 'a' && *s <= 'f')
			d = *s - 'a' + 10;
		else if (base == 16 && *s >= 'A' && *s <= 'F')
			d = *s - 'A' + 10;
		else
			return -1;
		r = r * base + d;
	}
	*v = neg ? -r : r;
	return 0;
}

int sh_read_key(void)
{
	int c;
	while ((c = input_getc()) < 0)
		sleep_ms(10);
	return c;
}

static void echo_str(const char *s)
{
	console_write(s, strlen(s));
}

static char preset[SH_LINE];
static char *edit_buf;
static int edit_len, edit_pos, edit_hidden;

static void echo_char(char c)
{
	char e = edit_hidden ? '*' : c;
	console_write(&e, 1);
}

static void back(int n)
{
	while (n-- > 0)
		console_write("\b", 1);
}

static void retype(int from, int extra)
{
	for (int i = from; i < edit_len; i++)
		echo_char(edit_buf[i]);
	for (int i = 0; i < extra; i++)
		console_write(" ", 1);
	back(edit_len - from + extra);
}

static void replace(const char *text, int room)
{
	back(edit_pos);
	int old = edit_len;
	edit_len = snprintf(edit_buf, room, "%s", text);
	if (edit_len >= room)
		edit_len = room - 1;
	edit_pos = 0;
	retype(0, old > edit_len ? old - edit_len : 0);
	for (; edit_pos < edit_len; edit_pos++)
		echo_char(edit_buf[edit_pos]);
}

static void insert_text(const char *s, int n, int room)
{
	if (edit_len + n >= room)
		n = room - 1 - edit_len;
	if (n <= 0)
		return;
	memmove(edit_buf + edit_pos + n, edit_buf + edit_pos, edit_len - edit_pos);
	memcpy(edit_buf + edit_pos, s, n);
	edit_len += n;
	for (int i = 0; i < n; i++)
		echo_char(edit_buf[edit_pos++]);
	retype(edit_pos, 0);
}

static void delete_at(int at)
{
	if (at < 0 || at >= edit_len)
		return;
	memmove(edit_buf + at, edit_buf + at + 1, edit_len - at - 1);
	edit_len--;
	if (at < edit_pos) {
		back(1);
		edit_pos--;
	}
	retype(edit_pos, 1);
}

void sh_complete(char *buf, int len, int pos, char *add, int room, int listing);

int sh_read_line(const char *prompt, char *buf, int room, int hidden)
{
	int hist = history_count;
	edit_buf = buf;
	edit_len = edit_pos = 0;
	edit_hidden = hidden;
	buf[0] = 0;
	if (prompt)
		echo_str(prompt);
	if (preset[0]) {
		insert_text(preset, strlen(preset), room);
		preset[0] = 0;
	}
	for (;;) {
		int c = sh_read_key();
		int plain = c & ~KEY_MODS;
		if (c == '\n' || c == KEY_SHIFT_ENTER) {
			for (; edit_pos < edit_len; edit_pos++)
				echo_char(buf[edit_pos]);
			echo_str("\n");
			buf[edit_len] = 0;
			return edit_len;
		}
		if (c == KEY_INTERRUPT) {
			echo_str("^C\n");
			buf[0] = 0;
			return -1;
		}
		if (c == 12 && !hidden) {		// ctrl l
			buf[0] = 0;
			terminal_clear();
			return -2;
		}
		if (c == KEY_ESC) {
			replace("", room);
			continue;
		}
		if (c == (KEY_F1) && !hidden) {
			replace("help", room);
			echo_str("\n");
			return edit_len;
		}
		if (plain == KEY_PAGE_UP || plain == KEY_PAGE_DOWN) {
			int page = terminal_rows() - 1;
			terminal_scroll(plain == KEY_PAGE_UP ? -page : page);
			continue;
		}
		if ((plain == KEY_UP || plain == KEY_DOWN) && !hidden) {
			int step = plain == KEY_UP ? -1 : 1;
			if (hist + step < 0 || hist + step > history_count)
				continue;
			hist += step;
			replace(hist < history_count ? history[hist] : "", room);
			continue;
		}
		if (plain == KEY_LEFT) {
			if (edit_pos > 0) {
				back(1);
				edit_pos--;
			}
			continue;
		}
		if (plain == KEY_RIGHT) {
			if (edit_pos < edit_len)
				echo_char(buf[edit_pos++]);
			continue;
		}
		if (plain == KEY_HOME || c == 1) {
			back(edit_pos);
			edit_pos = 0;
			continue;
		}
		if (plain == KEY_END || c == 5) {
			for (; edit_pos < edit_len; edit_pos++)
				echo_char(buf[edit_pos]);
			continue;
		}
		if (plain == KEY_DELETE) {
			delete_at(edit_pos);
			continue;
		}
		if (c == 8 || c == 127) {
			delete_at(edit_pos - 1);
			continue;
		}
		if (c == 21) {
			while (edit_pos)
				delete_at(edit_pos - 1);
			continue;
		}
		if (c == '\t' && !hidden) {
			char add[SH_LINE];
			buf[edit_len] = 0;
			sh_complete(buf, edit_len, edit_pos, add, sizeof add, 0);
			if (add[0]) {
				insert_text(add, strlen(add), room);
			} else {
				echo_str("\n");
				sh_complete(buf, edit_len, edit_pos, add, sizeof add, 1);
				buf[edit_len] = 0;
				snprintf(preset, sizeof preset, "%s", buf);
				return -3;
			}
			continue;
		}
		if (c >= ' ' && c < 127) {
			char ch = c;
			insert_text(&ch, 1, room);
		}
	}
}

// lds: enter runs the line under the cursor, wherever it is. ctrl up/down is history
static int read_command(char *buf, int room)
{
	int hist = history_count;
	terminal_mark_prompt();
	if (preset[0]) {
		terminal_replace_command(preset);
		preset[0] = 0;
	}
	for (;;) {
		int c = sh_read_key();
		int plain = c & ~KEY_MODS;
		if (c == '\n') {
			terminal_command(buf, room, NULL);
			terminal_to_end();
			for (char *s = buf; *s; s++)
				platform_putc(*s);
			echo_str("\n");
			int n = strlen(buf);
			while (n && buf[n - 1] == ' ')
				buf[--n] = 0;
			char *s = buf;
			while (*s == ' ')
				s++;
			memmove(buf, s, strlen(s) + 1);
			return strlen(buf);
		}
		if (c == KEY_INTERRUPT) {
			terminal_to_end();
			echo_str("^C\n");
			return -1;
		}
		if (c == 12) {
			terminal_clear();
			return -2;
		}
		if (c == KEY_F1) {
			terminal_to_end();
			echo_str("\n");
			strcpy(buf, "help");
			return 4;
		}
		if ((plain == KEY_UP || plain == KEY_DOWN) && (c & KEY_CTRL)) {
			int step = plain == KEY_UP ? -1 : 1;
			if (hist + step < 0 || hist + step > history_count)
				continue;
			hist += step;
			terminal_replace_command(hist < history_count ? history[hist] : "");
			continue;
		}
		if (c == 1 || c == 5) {
			terminal_type(c == 1 ? KEY_HOME : KEY_END);
			continue;
		}
		if (c == 21) {
			int pos;
			terminal_command(buf, room, &pos);
			terminal_replace_command(buf + pos);
			terminal_type(KEY_HOME);
			continue;
		}
		if (c == '\t') {
			char add[SH_LINE];
			int pos, n = terminal_command(buf, room, &pos);
			sh_complete(buf, n, pos, add, sizeof add, 0);
			if (add[0]) {
				for (char *a = add; *a; a++)
					terminal_type(*a);
				continue;
			}
			terminal_to_end();
			echo_str("\n");
			sh_complete(buf, n, pos, add, sizeof add, 1);
			snprintf(preset, sizeof preset, "%s", buf);
			return -3;
		}
		terminal_type(c);
	}
}

static void remember(const char *line)
{
	if (!*line || (history_count && !strcmp(history[history_count - 1], line)))
		return;
	if (history_count == HISTORY) {
		memmove(history[0], history[1], sizeof history[0] * (HISTORY - 1));
		history_count--;
	}
	snprintf(history[history_count++], SH_LINE, "%s", line);
}

void sh_history(void)
{
	for (int i = 0; i < history_count; i++)
		out("%4d  %s\n", i + 1, history[i]);
}

static const struct command *lookup(const char *name);

const struct command *sh_lookup(const char *name)
{
	return lookup(name);
}

static const struct command *lookup(const char *name)
{
	const struct command *tables[] = {cmd_core, cmd_fs, cmd_text, cmd_user, cmd_sys, cmd_source, cmd_games};
	for (size_t t = 0; t < sizeof tables / sizeof *tables; t++)
		for (const struct command *c = tables[t]; c->name; c++)
			if (!strcmp(c->name, name))
				return c;
	return NULL;
}

// spaces split, | > and >> are words of their own, quotes keep spaces. no escapes and no
// variables. TODO globbing, rm *.c would be nice
static int tokenize(char *s, char **argv, int max)
{
	static char store[SH_LINE * 2];
	int argc = 0, at = 0;
	while (*s && argc < max) {
		while (*s == ' ' || *s == '\t')
			s++;
		if (!*s)
			break;
		char *w = store + at;
		argv[argc++] = w;
		if (*s == '|') {
			strcpy(w, "|");
			at += 2;
			s++;
			continue;
		}
		if (*s == '>') {
			int two = s[1] == '>';
			strcpy(w, two ? ">>" : ">");
			at += two ? 3 : 2;
			s += two ? 2 : 1;
			continue;
		}
		while (*s && *s != ' ' && *s != '\t' && *s != '|' && *s != '>' && at < (int)sizeof store - 1) {
			if (*s == '"' || *s == '\'') {
				char q = *s++;
				while (*s && *s != q && at < (int)sizeof store - 1)
					store[at++] = *s++;
				if (*s)
					s++;
			} else {
				store[at++] = *s++;
			}
		}
		store[at++] = 0;
	}
	return argc;
}

int sh_run(char *line)
{
	char *argv[SH_ARGS + 4];
	int argc = tokenize(line, argv, SH_ARGS + 3);
	if (!argc)
		return 0;
	char *bufs[2] = {stage_a, stage_b};
	int which = 0;
	stdin_buf = NULL;
	stdin_len = 0;
	int status = 0;
	for (int start = 0; start < argc;) {
		int end = start;
		while (end < argc && strcmp(argv[end], "|"))
			end++;
		int last = end >= argc;
		const char *redirect = NULL;
		int append = 0;
		int n = end - start;
		for (int i = start; i < end; i++)
			if (!strcmp(argv[i], ">") || !strcmp(argv[i], ">>")) {
				append = argv[i][1] == '>';
				redirect = i + 1 < end ? argv[i + 1] : NULL;
				n = i - start;
				if (!redirect) {
					sh_error("%s: where to?", argv[i]);
					return -1;
				}
				break;
			}
		if (!n) {
			sh_error("syntax error near |");
			return -1;
		}
		char *stage[SH_ARGS + 1];
		for (int i = 0; i < n && i < SH_ARGS; i++)
			stage[i] = argv[start + i];
		stage[n] = NULL;
		const struct command *c = lookup(stage[0]);
		capture = (!last || redirect) ? bufs[which] : NULL;
		capture_len = 0;
		if (!c) {
			sh_error("%s: Unknown command. Type help.", stage[0]);
			status = -1;
		} else {
			out_color(WHITE);
			c->fn(n, stage);
		}
		char *produced = capture;
		int produced_len = capture_len;
		capture = NULL;
		if (redirect) {
			int r = append ? fs_append(redirect, produced, produced_len) : fs_write(redirect, produced, produced_len);
			if (r < 0)
				sh_fs_error(redirect, r);
			produced_len = 0;
		}
		stdin_buf = produced;
		stdin_len = produced_len;
		which ^= 1;
		start = end + 1;
	}
	stdin_buf = NULL;
	return status;
}

static void prompt(void)
{
	const struct user_info *u = users_current();
	terminal_color(LIGHT_CYAN);
	char p[300];
	snprintf(p, sizeof p, "%s@serpentis:", u->name);
	echo_str(p);
	terminal_color(YELLOW);
	snprintf(p, sizeof p, "%s%s ", fs_cwd(), u->uid ? "$" : "#");
	echo_str(p);
	terminal_color(WHITE);
}

int sh_logout;
int sh_restart;		// format wiped the users, every session has to go, su ones too

static const char welcome[] =
	" SSSSS EEEEE RRRR  PPPP  EEEEE N   N TTTTT IIIII SSSSS\n"
	" S     E     R   R P   P E     NN  N   T     I   S\n"
	" SSSSS EEEE  RRRR  PPPP  EEEE  N N N   T     I   SSSSS\n"
	"     S E     R  R  P     E     N  NN   T     I       S\n"
	" SSSSS EEEEE R   R P     EEEEE N   N   T   IIIII SSSSS\n\n";

// one login. su runs another one of these on top and comes back when it ends
void shell_session(void)
{
	char line[SH_LINE];
	sh_logout = 0;
	terminal_color(LIGHT_GREEN);
	echo_str(welcome);
	terminal_color(WHITE);
	while (!sh_logout && !sh_restart) {
		prompt();
		if (read_command(line, sizeof line) < 0)
			continue;
		remember(line);
		sh_run(line);
	}
}

void shell_main(void *arg)
{
	login_run();
}
