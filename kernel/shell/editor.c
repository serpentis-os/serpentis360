#include "kernel.h"
#include "ppc.h"
#include "screen.h"
#include "usb.h"
#include "shell.h"
#include "keys.h"

// the serpentis-os editor in C, one flat buffer
// TODO no buffers and no file tree yet, the asm one has both

enum { NORMAL, INSERT, VISUAL, VLINE, COMMAND, SEARCH };
enum { LANG_TEXT, LANG_C, LANG_ASM };

#define TEXT_MAX FS_FILE_MAX
#define UNDO_MAX 4096
#define UNDO_TEXT (512 * 1024)

static char text[TEXT_MAX];
static int len, cur, top, left;
static int mode, modified, lang;
static int show_numbers = 1, relative;
static char path[FS_PATH_MAX];
static char message[160];
static int message_error;
static char line_in[160];
static int line_in_len;
static char search[80];
static int highlight_search;
static char yank[TEXT_MAX / 4];
static int yank_len, yank_lines;
static int vstart;
static int quit_now, quit_asked;
static int want_col = -1;
static char last_find, last_find_kind;

struct undo {
	int group;
	int pos;
	int del_at, del_len;
	int ins_at, ins_len;
};

static struct undo undos[UNDO_MAX];
static char undo_text[UNDO_TEXT];
static int undo_count, undo_used, undo_top, group;

// undo keeps deleted and inserted text in one buffer. when that fills up, 4096 changes or
// 512 KB of text, the history is dropped and starts over
static int keep(const char *s, int n)
{
	if (undo_used + n > UNDO_TEXT || undo_count == UNDO_MAX) {
		undo_count = undo_top = undo_used = 0;
		return -1;
	}
	memcpy(undo_text + undo_used, s, n);
	undo_used += n;
	return undo_used - n;
}

static void record(int pos, const char *del, int dn, const char *ins, int in)
{
	undo_count = undo_top;
	struct undo *u = &undos[undo_count];
	u->group = group;
	u->pos = pos;
	u->del_at = dn ? keep(del, dn) : 0;
	u->del_len = dn;
	u->ins_at = in ? keep(ins, in) : 0;
	u->ins_len = in;
	if ((dn && u->del_at < 0) || (in && u->ins_at < 0))
		return;
	undo_top = ++undo_count;
}

static void raw_insert(int pos, const char *s, int n)
{
	if (len + n > TEXT_MAX)
		n = TEXT_MAX - len;
	if (n <= 0)
		return;
	memmove(text + pos + n, text + pos, len - pos);
	memcpy(text + pos, s, n);
	len += n;
	modified = 1;
}

static void raw_delete(int pos, int n)
{
	if (n <= 0)
		return;
	memmove(text + pos, text + pos + n, len - pos - n);
	len -= n;
	modified = 1;
}

static int changes;

static void insert(int pos, const char *s, int n)
{
	changes++;
	record(pos, NULL, 0, s, n);
	raw_insert(pos, s, n);
}

static void erase(int pos, int n)
{
	if (pos < 0)
		n += pos, pos = 0;
	if (pos + n > len)
		n = len - pos;
	if (n <= 0)
		return;
	changes++;
	record(pos, text + pos, n, NULL, 0);
	raw_delete(pos, n);
}

static void undo(void)
{
	if (!undo_top) {
		snprintf(message, sizeof message, "Already at oldest change");
		return;
	}
	int g = undos[undo_top - 1].group;
	while (undo_top && undos[undo_top - 1].group == g) {
		struct undo *u = &undos[--undo_top];
		raw_delete(u->pos, u->ins_len);
		raw_insert(u->pos, undo_text + u->del_at, u->del_len);
		cur = u->pos;
	}
}

static void redo(void)
{
	if (undo_top == undo_count) {
		snprintf(message, sizeof message, "Already at newest change");
		return;
	}
	int g = undos[undo_top].group;
	while (undo_top < undo_count && undos[undo_top].group == g) {
		struct undo *u = &undos[undo_top++];
		raw_delete(u->pos, u->del_len);
		raw_insert(u->pos, undo_text + u->ins_at, u->ins_len);
		cur = u->pos + u->ins_len;
	}
}

static int line_start(int at)
{
	while (at > 0 && text[at - 1] != '\n')
		at--;
	return at;
}

static int line_end(int at)
{
	while (at < len && text[at] != '\n')
		at++;
	return at;
}

static int line_of(int at)
{
	int n = 0;
	for (int i = 0; i < at && i < len; i++)
		n += text[i] == '\n';
	return n;
}

static int line_count(void)
{
	return line_of(len) + 1;
}

static int offset_of_line(int line)
{
	int at = 0;
	while (line > 0 && at < len)
		if (text[at++] == '\n')
			line--;
	return at;
}

static int column(int at)
{
	return at - line_start(at);
}

static int disp_col(int at)
{
	int x = 0;
	for (int i = line_start(at); i < at; i++)
		x += text[i] == '\t' ? 4 - x % 4 : 1;
	return x;
}

static int clamp_normal(int at)
{
	int s = line_start(at), e = line_end(at);
	if (mode != INSERT && e > s && at >= e)
		at = e - 1;
	return at < 0 ? 0 : at > len ? len : at;
}

static int first_nonblank(int at)
{
	int s = line_start(at);
	while (s < len && (text[s] == ' ' || text[s] == '\t'))
		s++;
	return s;
}

static int to_line_col(int line, int col)
{
	int s = offset_of_line(line), e = line_end(s);
	int at = s + col;
	if (at > e)
		at = e;
	return at;
}

static int cls(char c)
{
	if (c == ' ' || c == '\t' || c == '\n')
		return 0;
	if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')
		return 1;
	return 2;
}

static int big(char c)
{
	return c == ' ' || c == '\t' || c == '\n' ? 0 : 1;
}

static int word_forward(int at, int bigword)
{
	int (*k)(char) = bigword ? big : cls;
	int c = at < len ? k(text[at]) : 0;
	while (at < len && c && k(text[at]) == c)
		at++;
	while (at < len && !k(text[at]))
		at++;
	return at;
}

static int word_back(int at, int bigword)
{
	int (*k)(char) = bigword ? big : cls;
	if (at > 0)
		at--;
	while (at > 0 && !k(text[at]))
		at--;
	int c = k(text[at]);
	while (at > 0 && k(text[at - 1]) == c)
		at--;
	return at;
}

static int word_end(int at, int bigword)
{
	int (*k)(char) = bigword ? big : cls;
	if (at < len - 1)
		at++;
	while (at < len - 1 && !k(text[at]))
		at++;
	int c = k(text[at]);
	while (at < len - 1 && k(text[at + 1]) == c)
		at++;
	return at;
}

static int paragraph(int at, int dir)
{
	int line = line_of(at), n = line_count();
	do {
		line += dir;
	} while (line > 0 && line < n - 1 && line_start(offset_of_line(line)) != line_end(offset_of_line(line)));
	if (line < 0)
		line = 0;
	if (line > n - 1)
		line = n - 1;
	return offset_of_line(line);
}

static int match_bracket(int at)
{
	const char *open = "([{", *close = ")]}";
	int s = at, e = line_end(at);
	while (s < e && !strchr("()[]{}", text[s]))
		s++;
	if (s >= e)
		return at;
	char c = text[s];
	const char *o = strchr(open, c);
	int dir = o ? 1 : -1;
	char want = o ? close[o - open] : open[strchr(close, c) - close];
	int depth = 0;
	for (int i = s; i >= 0 && i < len; i += dir) {
		if (text[i] == c)
			depth++;
		else if (text[i] == want && !--depth)
			return i;
	}
	return at;
}

static int find_char(int at, char kind, char c)
{
	int s = line_start(at), e = line_end(at);
	if (kind == 'f' || kind == 't') {
		for (int i = at + 1 + (kind == 't'); i < e; i++)
			if (text[i] == c)
				return kind == 't' ? i - 1 : i;
	} else {
		for (int i = at - 1 - (kind == 'T'); i >= s; i--)
			if (text[i] == c)
				return kind == 'T' ? i + 1 : i;
	}
	return at;
}

static int search_from(int at, int dir)
{
	int n = strlen(search);
	if (!n)
		return at;
	for (int k = 1; k <= len; k++) {
		int i = ((at + dir * k) % len + len) % len;
		if (i + n <= len && !strncmp(text + i, search, n))
			return i;
	}
	message_error = 1, snprintf(message, sizeof message, "Pattern not found: %s", search);
	return at;
}

// every key gets recorded so . can feed it in again

#define REC_MAX 512

static int replay[REC_MAX], replay_len, replay_at, from_replay;
static int rec[REC_MAX], rec_len, recording;
static int last_change[REC_MAX], last_change_len, rec_from;

static int key(void)
{
	int c;
	from_replay = replay_at < replay_len;
	if (from_replay) {
		c = replay[replay_at++];
	} else {
		c = sh_read_key();
		int plain = c & ~KEY_MODS;
		if (plain == KEY_PAGE_UP)
			c = 2;
		else if (plain == KEY_PAGE_DOWN)
			c = 6;
		else if ((plain >= KEY_UP && plain <= KEY_END) || plain == KEY_DELETE)
			c = plain;
		else if (c == KEY_SHIFT_ENTER)
			c = '\n';
		else if (c & KEY_ALT)
			c &= 0xff;
	}
	if (recording && rec_len < REC_MAX)
		rec[rec_len++] = c;
	return c;
}

static const char *c_keywords[] = {
	"auto", "break", "case", "char", "const", "continue", "default", "do", "double", "else", "enum",
	"extern", "float", "for", "goto", "if", "int", "long", "register", "return", "short", "signed",
	"sizeof", "static", "struct", "switch", "typedef", "union", "unsigned", "void", "volatile", "while",
	"uint8_t", "uint16_t", "uint32_t", "uint64_t", "int8_t", "int16_t", "int32_t", "int64_t", "size_t",
	"bool", "true", "false", "NULL", "inline", NULL
};

static const char *asm_directives[] = {
	"db", "dw", "dd", "dq", "rb", "rw", "rd", "rq", "times", "include", "macro", "equ", "format",
	"section", "segment", "align", "use64", "use32", "use16", "org", "if", "else", "end", "repeat",
	"local", "define", "file", "virtual", "struc", NULL
};

static int in_list(const char **list, const char *w, int n)
{
	for (; *list; list++)
		if ((int)strlen(*list) == n && !strncmp(*list, w, n))
			return 1;
	return 0;
}

static int is_register(const char *w, int n)
{
	static const char *regs[] = {
		"rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp", "eax", "ebx", "ecx", "edx", "esi",
		"edi", "ebp", "esp", "ax", "bx", "cx", "dx", "si", "di", "bp", "sp", "al", "bl", "cl", "dl",
		"ah", "bh", "ch", "dh", "sil", "dil", NULL
	};
	if (in_list(regs, w, n))
		return 1;
	if ((w[0] == 'r' || w[0] == 'R') && n >= 2 && n <= 4 && w[1] >= '0' && w[1] <= '9')
		return 1;
	return n >= 4 && !strncmp(w, "xmm", 3);
}

// one line at a time, the only thing carried over is being inside a /* comment
static void color_line(int s, int e, uint8_t *colors, int *in_comment)
{
	for (int i = s; i < e; i++)
		colors[i - s] = WHITE;
	if (lang == LANG_TEXT)
		return;
	int first_word = 1;
	for (int i = s; i < e;) {
		char c = text[i];
		if (lang == LANG_C && *in_comment) {
			colors[i - s] = DARK_GRAY;
			if (c == '*' && i + 1 < e && text[i + 1] == '/') {
				colors[i + 1 - s] = DARK_GRAY;
				*in_comment = 0;
				i += 2;
			} else {
				i++;
			}
			continue;
		}
		if ((lang == LANG_C && c == '/' && i + 1 < e && text[i + 1] == '/') || (lang == LANG_ASM && c == ';')) {
			for (; i < e; i++)
				colors[i - s] = DARK_GRAY;
			break;
		}
		if (lang == LANG_C && c == '/' && i + 1 < e && text[i + 1] == '*') {
			*in_comment = 1;
			continue;
		}
		if (lang == LANG_C && c == '#' && first_word) {
			for (; i < e; i++)
				colors[i - s] = LIGHT_GREEN;
			break;
		}
		if (c == '"' || c == '\'') {
			int j = i + 1;
			while (j < e && text[j] != c) {
				if (text[j] == '\\')
					j++;
				j++;
			}
			for (int k = i; k <= j && k < e; k++)
				colors[k - s] = YELLOW;
			i = j + 1;
			continue;
		}
		if (c >= '0' && c <= '9') {
			int j = i;
			while (j < e && cls(text[j]) == 1)
				colors[j++ - s] = LIGHT_MAGENTA;
			i = j;
			continue;
		}
		if (cls(c) == 1 || c == '.' || c == '%') {
			int j = i;
			while (j < e && (cls(text[j]) == 1 || text[j] == '.' || text[j] == '%'))
				j++;
			const char *w = text + i + (c == '%');
			int n = j - i - (c == '%');
			int color = WHITE;
			if (lang == LANG_C) {
				if (in_list(c_keywords, w, n))
					color = LIGHT_CYAN;
			} else {
				if (j < e && text[j] == ':' && first_word)
					color = YELLOW;
				else if (in_list(asm_directives, w, n))
					color = LIGHT_GREEN;
				else if (is_register(w, n))
					color = LIGHT_CYAN;
				else if (first_word)
					color = LIGHT_BLUE;
			}
			for (int k = i; k < j; k++)
				colors[k - s] = color;
			if (!(lang == LANG_ASM && j < e && text[j] == ':'))
				first_word = 0;
			i = j;
			continue;
		}
		i++;
	}
}

static int gutter(void)
{
	return show_numbers || relative ? 5 : 0;
}

static void scroll_to_cursor(int rows, int width)
{
	int line = line_of(cur);
	if (line < top)
		top = line;
	if (line >= top + rows)
		top = line - rows + 1;
	int col = disp_col(cur);
	if (col < left)
		left = col;
	if (col >= left + width)
		left = col - width + 1;
}

static void draw(void)
{
	int cols = terminal_cols(), rows = terminal_rows() - 2;
	int g = gutter(), width = cols - g;
	scroll_to_cursor(rows, width);
	int in_comment = 0;
	if (lang == LANG_C)
		for (int i = 0, end = offset_of_line(top); i < end; i++) {
			if (!in_comment && text[i] == '/' && i + 1 < end && text[i + 1] == '*')
				in_comment = 1, i++;
			else if (in_comment && text[i] == '*' && i + 1 < end && text[i + 1] == '/')
				in_comment = 0, i++;
			else if (!in_comment && text[i] == '/' && i + 1 < end && text[i + 1] == '/')
				i = line_end(i);
		}
	int vs = 0, ve = -1;
	if (mode == VISUAL || mode == VLINE) {
		vs = vstart < cur ? vstart : cur;
		ve = vstart < cur ? cur : vstart;
		if (mode == VLINE) {
			vs = line_start(vs);
			ve = line_end(ve);
		}
	}
	int lines = line_count(), cur_line = line_of(cur);
	int at = offset_of_line(top);
	static uint8_t colors[TEXT_MAX / 64];
	int nlen = strlen(search), lit = 0;
	for (int y = 0; y < rows; y++) {
		int line = top + y;
		if (line >= lines) {
			for (int x = 0; x < cols; x++)
				terminal_cell(x, y, x == 0 ? '~' : ' ', BLUE, BLACK);
			continue;
		}
		if (g) {
			char num[8];
			int shown_num = relative && line != cur_line ? (line > cur_line ? line - cur_line : cur_line - line) : line + 1;
			snprintf(num, sizeof num, "%4d ", shown_num);
			for (int x = 0; x < g; x++)
				terminal_cell(x, y, num[x], line == cur_line ? YELLOW : DARK_GRAY, BLACK);
		}
		int s = at, e = line_end(at);
		lit = 0;
		int n = e - s < (int)sizeof colors ? e - s : (int)sizeof colors;
		color_line(s, s + n, colors, &in_comment);
		int x = 0;
		for (int i = s; i <= e && x - left < width; i++) {
			int w = i < e && text[i] == '\t' ? 4 - x % 4 : 1;
			int fg = i < e && i - s < n ? colors[i - s] : WHITE, bg = BLACK;
			if (i >= vs && i <= ve && (i < e || mode == VLINE))
				bg = BLUE, fg = WHITE;
			if (highlight_search && nlen && i < e && i + nlen <= len && !strncmp(text + i, search, nlen))
				lit = nlen;
			if (lit > 0 && i < e) {
				bg = BROWN;
				lit--;
			}
			char ch = i < e && text[i] != '\t' ? text[i] : ' ';
			for (int k = 0; k < w; k++, x++)
				if (x >= left && x - left < width)
					terminal_cell(g + x - left, y, ch, fg, bg);
		}
		for (int k = x > left ? x - left : 0; k < width; k++)
			terminal_cell(g + k, y, ' ', WHITE, BLACK);
		at = e + 1;
	}

	static const char *names[] = {"NORMAL", "INSERT", "VISUAL", "V-LINE", "NORMAL", "NORMAL"};
	char status[160];
	int n = snprintf(status, sizeof status, " %s  %s%s", names[mode], path[0] ? path : "[No Name]", modified ? " [+]" : "");
	char right[40];
	snprintf(right, sizeof right, "%d:%d  %d lines ", cur_line + 1, disp_col(cur) + 1, lines);
	for (int x = 0; x < cols; x++) {
		char ch = x < n ? status[x] : ' ';
		int r = cols - (int)strlen(right);
		if (x >= r)
			ch = right[x - r];
		int bg = mode == INSERT ? GREEN : mode == VISUAL || mode == VLINE ? MAGENTA : BLUE;
		terminal_cell(x, rows, ch, WHITE, x < 8 ? bg : DARK_GRAY);
	}
	const char *bottom = message;
	char typed[170];
	if (mode == COMMAND || mode == SEARCH) {
		snprintf(typed, sizeof typed, "%c%s", mode == COMMAND ? ':' : '/', line_in);
		bottom = typed;
	}
	int bl = strlen(bottom);
	for (int x = 0; x < cols; x++)
		terminal_cell(x, rows + 1, x < bl ? bottom[x] : ' ', message_error && bottom == message ? LIGHT_RED : WHITE, BLACK);
	if (mode == COMMAND || mode == SEARCH)
		terminal_goto(bl < cols ? bl : cols - 1, rows + 1);
	else
		terminal_goto(g + disp_col(cur) - left, cur_line - top);
}

static int detect_language(const char *p)
{
	const char *dot = strrchr(p, '.');
	if (!dot)
		return LANG_TEXT;
	if (!strcmp(dot, ".c") || !strcmp(dot, ".h"))
		return LANG_C;
	if (!strcmp(dot, ".asm") || !strcmp(dot, ".inc") || !strcmp(dot, ".s") || !strcmp(dot, ".S"))
		return LANG_ASM;
	return LANG_TEXT;
}

static int load(const char *p)
{
	int n = fs_read(p, text, TEXT_MAX);
	if (n == FS_ENOENT) {
		len = 0;
		snprintf(message, sizeof message, "\"%s\" [New]", p);
	} else if (n < 0) {
		snprintf(message, sizeof message, "%s: %s", p, fs_error(n));
		return n;
	} else {
		len = n;
		snprintf(message, sizeof message, "\"%s\" %dL, %dB", p, line_count(), len);
	}
	fs_resolve(p, path);
	lang = detect_language(path);
	cur = top = left = 0;
	modified = 0;
	undo_count = undo_top = undo_used = 0;
	return 0;
}

static int save(const char *p)
{
	if (p && *p)
		fs_resolve(p, path);
	if (!path[0]) {
		message_error = 1, snprintf(message, sizeof message, "No file name");
		return -1;
	}
	if (len && text[len - 1] != '\n' && len < TEXT_MAX)
		text[len++] = '\n';
	int r = fs_write(path, text, len);
	if (r < 0) {
		snprintf(message, sizeof message, "%s: %s", path, fs_error(r));
		return r;
	}
	modified = 0;
	lang = detect_language(path);
	snprintf(message, sizeof message, "\"%s\" %dL, %dB written", path, line_count(), len);
	return 0;
}

// plain text, no regex
static void substitute(char *cmd, int whole)
{
	char sep = cmd[0];
	char *a = cmd + 1, *b = strchr(a, sep);
	if (!b) {
		message_error = 1, snprintf(message, sizeof message, "E: :s/old/new/[g]");
		return;
	}
	*b++ = 0;
	char *f = strchr(b, sep);
	int global = 0;
	if (f) {
		*f++ = 0;
		global = strchr(f, 'g') != NULL;
	}
	int na = strlen(a), nb = strlen(b), count = 0;
	if (!na)
		return;
	group++;
	int s = whole ? 0 : line_start(cur), e = whole ? len : line_end(cur);
	for (int i = s; i + na <= e;) {
		if (strncmp(text + i, a, na)) {
			i++;
			continue;
		}
		erase(i, na);
		insert(i, b, nb);
		e += nb - na;
		count++;
		i += nb;
		if (!global) {
			int le = line_end(i);
			i = le + 1;
			if (!whole)
				break;
		}
	}
	snprintf(message, sizeof message, "%d substitution%s", count, count == 1 ? "" : "s");
}

static void ex(char *cmd)
{
	while (*cmd == ' ')
		cmd++;
	char *arg = strchr(cmd, ' ');
	if (arg) {
		*arg++ = 0;
		while (*arg == ' ')
			arg++;
	}
	long n;
	if (!*cmd)
		return;
	if (!sh_number(cmd, &n)) {
		int lines = line_count();
		cur = first_nonblank(offset_of_line(n < 1 ? 0 : n > lines ? lines - 1 : n - 1));
	} else if (!strcmp(cmd, "w") || !strcmp(cmd, "write")) {
		save(arg);
	} else if (!strcmp(cmd, "wq") || !strcmp(cmd, "x")) {
		if (!modified && !strcmp(cmd, "x"))
			quit_now = 1;
		else if (save(arg) == 0)
			quit_now = 1;
	} else if (!strcmp(cmd, "q") || !strcmp(cmd, "quit") || !strcmp(cmd, "qa")) {
		if (modified)
			message_error = 1, snprintf(message, sizeof message, "E37: No write since last change (:q! or ctrl+q twice to drop it)");
		else
			quit_now = 1;
	} else if (!strcmp(cmd, "q!") || !strcmp(cmd, "qa!")) {
		quit_now = 1;
	} else if (!strcmp(cmd, "e") || !strcmp(cmd, "edit") || !strcmp(cmd, "e!")) {
		if (modified && cmd[1] != '!')
			message_error = 1, snprintf(message, sizeof message, "E37: No write since last change (add ! to override)");
		else if (arg && *arg)
			load(arg);
		else if (path[0])
			load(path);
	} else if (!strcmp(cmd, "set") && arg) {
		if (!strcmp(arg, "nu") || !strcmp(arg, "number"))
			show_numbers = 1;
		else if (!strcmp(arg, "nonu") || !strcmp(arg, "nonumber"))
			show_numbers = 0;
		else if (!strcmp(arg, "rnu"))
			relative = 1;
		else if (!strcmp(arg, "nornu"))
			relative = 0;
		else
			message_error = 1, snprintf(message, sizeof message, "E518: Unknown option: %s", arg);
	} else if (!strcmp(cmd, "noh")) {
		highlight_search = 0;
	} else if (cmd[0] == 's' && cmd[1] && !arg) {
		substitute(cmd + 1, 0);
	} else if (cmd[0] == '%' && cmd[1] == 's') {
		if (arg)
			arg[-1] = ' ';
		substitute(cmd + 2, 1);
	} else {
		message_error = 1, snprintf(message, sizeof message, "E492: Not an editor command: %s", cmd);
	}
}

static void set_yank(int s, int e, int lines)
{
	int n = e - s;
	if (n > (int)sizeof yank)
		n = sizeof yank;
	memcpy(yank, text + s, n);
	yank_len = n;
	yank_lines = lines;
}

static void indent(int s, int e, int dir)
{
	group++;
	for (int at = line_start(s); at <= e && at < len;) {
		if (dir > 0) {
			insert(at, "    ", 4);
			e += 4;
		} else {
			int k = 0;
			while (k < 4 && at + k < len && text[at + k] == ' ')
				k++;
			erase(at, k);
			e -= k;
		}
		int le = line_end(at);
		if (le >= len)
			break;
		at = le + 1;
	}
}

static void apply(char op, int s, int e, int lines)
{
	if (s > e) {
		int t = s;
		s = e;
		e = t;
	}
	if (lines) {
		s = line_start(s);
		e = line_end(e);
		if (e < len)
			e++;
		else if (s > 0 && op != 'y' && op != '>' && op != '<')
			s--;
	}
	if (op == 'y') {
		set_yank(s, e, lines);
		cur = s;
		return;
	}
	if (op == '>' || op == '<') {
		indent(s, e > s ? e - 1 : e, op == '>' ? 1 : -1);
		cur = first_nonblank(s);
		return;
	}
	group++;
	set_yank(s, e, lines);
	erase(s, e - s);
	cur = s;
	if (op == 'c') {
		if (lines)
			insert(s, "\n", 1);
		mode = INSERT;
	}
}

// iw aw ip ap and the bracket and quote ones
static int text_object(char around, char what, int *s, int *e)
{
	if (what == 'w' || what == 'W') {
		int (*k)(char) = what == 'W' ? big : cls;
		int a = cur, b = cur;
		int c = k(text[cur]);
		while (a > 0 && k(text[a - 1]) == c && text[a - 1] != '\n')
			a--;
		while (b < len && k(text[b]) == c && text[b] != '\n')
			b++;
		if (around)
			while (b < len && (text[b] == ' ' || text[b] == '\t'))
				b++;
		*s = a;
		*e = b;
		return 0;
	}
	char open = what, close = what;
	if (what == '(' || what == ')' || what == 'b')
		open = '(', close = ')';
	else if (what == '{' || what == '}' || what == 'B')
		open = '{', close = '}';
	else if (what == '[' || what == ']')
		open = '[', close = ']';
	else if (what == '<' || what == '>')
		open = '<', close = '>';
	else if (what != '"' && what != '\'' && what != '`')
		return -1;
	int a, b;
	if (open == close) {
		int ls = line_start(cur), le = line_end(cur);
		a = cur;
		while (a > ls && text[a] != open)
			a--;
		if (text[a] != open)
			return -1;
		b = a + 1;
		while (b < le && text[b] != close)
			b++;
		if (b >= le)
			return -1;
	} else {
		int depth = 0;
		for (a = cur; a >= 0; a--) {
			if (text[a] == close && a != cur)
				depth++;
			else if (text[a] == open && depth-- == 0)
				break;
		}
		if (a < 0)
			return -1;
		depth = 0;
		for (b = a + 1; b < len; b++) {
			if (text[b] == open)
				depth++;
			else if (text[b] == close && depth-- == 0)
				break;
		}
		if (b >= len)
			return -1;
	}
	*s = around ? a : a + 1;
	*e = around ? b + 1 : b;
	return 0;
}

static int motion(int k, int count, int *linewise, int *inclusive, int *ok)
{
	int at = cur;
	*linewise = *inclusive = 0;
	*ok = 1;
	int line = line_of(cur), col = want_col >= 0 ? want_col : column(cur), lines = line_count();
	switch (k) {
	case 'h': case 0x103: case 8:
		for (int i = 0; i < count && at > line_start(cur); i++)
			at--;
		return at;
	case 'l': case 0x102: case ' ':
		for (int i = 0; i < count && at < line_end(cur) - (mode == NORMAL ? 1 : 0); i++)
			at++;
		return at;
	case 'j': case 0x101: case '+':
		*linewise = 1;
		line = line + count < lines ? line + count : lines - 1;
		return to_line_col(line, col);
	case 'k': case 0x100: case '-':
		*linewise = 1;
		line = line - count > 0 ? line - count : 0;
		return to_line_col(line, col);
	case 'w': case 'W':
		for (int i = 0; i < count; i++)
			at = word_forward(at, k == 'W');
		return at;
	case 'b': case 'B':
		for (int i = 0; i < count; i++)
			at = word_back(at, k == 'B');
		return at;
	case 'e': case 'E':
		*inclusive = 1;
		for (int i = 0; i < count; i++)
			at = word_end(at, k == 'E');
		return at;
	case '0': case 0x104:
		return line_start(cur);
	case '^':
		return first_nonblank(cur);
	case '$': case 0x105:
		*inclusive = 1;
		at = line_end(cur);
		return at > line_start(cur) ? at - 1 : at;
	case 'G':
		*linewise = 1;
		return first_nonblank(offset_of_line(count > 1 || want_col == -2 ? count - 1 : lines - 1));
	case '}':
		for (int i = 0; i < count; i++)
			at = paragraph(at, 1);
		return at;
	case '{':
		for (int i = 0; i < count; i++)
			at = paragraph(at, -1);
		return at;
	case '%':
		*inclusive = 1;
		return match_bracket(cur);
	case 'n': case 'N':
		highlight_search = 1;
		return search_from(cur, k == 'n' ? 1 : -1);
	case 'f': case 'F': case 't': case 'T': {
		int c = key();
		last_find = c;
		last_find_kind = k;
		*inclusive = k == 'f' || k == 't';
		for (int i = 0; i < count; i++)
			at = find_char(at, k, c);
		return at;
	}
	case ';': case ',': {
		char kind = last_find_kind;
		if (!kind)
			break;
		if (k == ',')
			kind = kind == 'f' ? 'F' : kind == 'F' ? 'f' : kind == 't' ? 'T' : 't';
		*inclusive = kind == 'f' || kind == 't';
		for (int i = 0; i < count; i++)
			at = find_char(at, kind, last_find);
		return at;
	}
	case 'g': {
		int c = key();
		if (c == 'g') {
			*linewise = 1;
			return first_nonblank(offset_of_line(count > 1 ? count - 1 : 0));
		}
		break;
	}
	case 4: case 21: case 6: case 2: {
		int rows = terminal_rows() - 2;
		int step = k == 4 || k == 21 ? rows / 2 : rows;
		*linewise = 1;
		line += k == 4 || k == 6 ? step : -step;
		line = line < 0 ? 0 : line >= lines ? lines - 1 : line;
		top += k == 4 || k == 6 ? step : -step;
		if (top < 0)
			top = 0;
		return to_line_col(line, col);
	}
	}
	*ok = 0;
	return cur;
}

static void paste(int before, int count)
{
	if (!yank_len)
		return;
	group++;
	for (int i = 0; i < count; i++) {
		if (yank_lines) {
			int at = before ? line_start(cur) : line_end(cur);
			if (!before) {
				if (at >= len) {
					insert(at, "\n", 1);
					at++;
					insert(at, yank, yank_len && yank[yank_len - 1] == '\n' ? yank_len - 1 : yank_len);
				} else {
					insert(at + 1, yank, yank_len);
				}
				cur = first_nonblank(at + 1);
			} else {
				insert(at, yank, yank_len);
				cur = first_nonblank(at);
			}
		} else {
			int at = before || cur >= len ? cur : cur + 1;
			if (!before && at > line_end(cur))
				at = line_end(cur);
			insert(at, yank, yank_len);
			cur = at + yank_len - 1;
		}
	}
}

static void open_line(int below)
{
	group++;
	int s = line_start(cur), fn = first_nonblank(cur);
	char ind[64];
	int n = fn - s < 63 ? fn - s : 63;
	memcpy(ind, text + s, n);
	if (below) {
		int e = line_end(cur);
		insert(e, "\n", 1);
		insert(e + 1, ind, n);
		cur = e + 1 + n;
	} else {
		insert(s, ind, n);
		insert(s + n, "\n", 1);
		cur = s + n;
	}
	mode = INSERT;
}

static void insert_key(int c)
{
	if (c == 27) {
		mode = NORMAL;
		if (cur > line_start(cur))
			cur--;
		return;
	}
	if (c == 0x100 || c == 0x101 || c == 0x102 || c == 0x103 || c == 0x104 || c == 0x105) {
		int lw, inc, ok;
		cur = motion(c, 1, &lw, &inc, &ok);
		return;
	}
	if (c == 8 || c == 127) {
		if (cur > 0) {
			erase(cur - 1, 1);
			cur--;
		}
		return;
	}
	if (c == KEY_DELETE) {
		if (cur < len)
			erase(cur, 1);
		return;
	}
	if (c == 23 || c == 21) {
		int s = line_start(cur), at = cur;
		if (c == 21)
			at = s;
		else {
			while (at > s && (text[at - 1] == ' ' || text[at - 1] == '\t'))
				at--;
			while (at > s && cls(text[at - 1]) == cls(text[at > s ? at - 1 : at]) && cls(text[at - 1]))
				at--;
		}
		erase(at, cur - at);
		cur = at;
		return;
	}
	if (c == '\r' || c == '\n') {
		int s = line_start(cur), fn = first_nonblank(cur);
		int n = fn - s < cur - s ? fn - s : cur - s;
		char ind[64];
		if (n > 63)
			n = 63;
		memcpy(ind, text + s, n);
		insert(cur, "\n", 1);
		insert(cur + 1, ind, n);
		cur += 1 + n;
		return;
	}
	if (c == '\t') {
		insert(cur, "    ", 4);
		cur += 4;
		return;
	}
	if (c >= ' ' && c < 127) {
		char ch = c;
		insert(cur, &ch, 1);
		cur++;
	}
}

static void line_input_key(int c)
{
	if (c == 27) {
		mode = NORMAL;
		return;
	}
	if (c == 8 || c == 127) {
		if (line_in_len)
			line_in[--line_in_len] = 0;
		else
			mode = NORMAL;
		return;
	}
	if (c == '\r' || c == '\n') {
		int was = mode;
		mode = NORMAL;
		message_error = 0;
		if (was == COMMAND) {
			ex(line_in);
		} else {
			if (line_in_len)
				snprintf(search, sizeof search, "%s", line_in);
			highlight_search = 1;
			cur = search_from(cur, 1);
		}
		return;
	}
	if (c >= ' ' && c < 127 && line_in_len < (int)sizeof line_in - 1) {
		line_in[line_in_len++] = c;
		line_in[line_in_len] = 0;
	}
}

static void start_line_input(int m)
{
	mode = m;
	line_in_len = 0;
	line_in[0] = 0;
	message[0] = 0;
}

static void visual_key(int c, int count)
{
	int s = vstart < cur ? vstart : cur, e = vstart < cur ? cur : vstart;
	switch (c) {
	case 27: case 'v': case 'V':
		if ((c == 'v' && mode == VLINE) || (c == 'V' && mode == VISUAL)) {
			mode = c == 'v' ? VISUAL : VLINE;
			return;
		}
		mode = NORMAL;
		return;
	case 'o': {
		int t = vstart;
		vstart = cur;
		cur = t;
		return;
	}
	case 'd': case 'x': case 'y': case 'c': case '>': case '<': {
		int lines = mode == VLINE;
		mode = NORMAL;
		char op = c == 'x' ? 'd' : c;
		apply(op, s, lines ? e : e + 1, lines);
		return;
	}
	case ':':
		start_line_input(COMMAND);
		return;
	}
	int lw, inc, ok;
	int to = motion(c, count, &lw, &inc, &ok);
	if (ok)
		cur = clamp_normal(to);
}

static void normal_key(int c)
{
	int count = 0;
	while ((c >= '1' && c <= '9') || (count && c == '0')) {
		count = count * 10 + c - '0';
		c = key();
	}
	int had_count = count > 0;
	if (!count)
		count = 1;
	message[0] = 0;

	if (mode == VISUAL || mode == VLINE) {
		visual_key(c, count);
		return;
	}

	switch (c) {
	case 19: save(NULL); return;
	case '*': case '#': {
		int a = cur, b = cur;
		while (a > 0 && cls(text[a - 1]) == 1)
			a--;
		while (b < len && cls(text[b]) == 1)
			b++;
		if (b > a && b - a < (int)sizeof search) {
			memcpy(search, text + a, b - a);
			search[b - a] = 0;
			highlight_search = 1;
			cur = search_from(c == '*' ? b - 1 : a, c == '*' ? 1 : -1);
		}
		return;
	}
	case 'H': case 'M': case 'L': {
		int rows = terminal_rows() - 2, last = line_count() - 1;
		int bottom = top + rows - 1 < last ? top + rows - 1 : last;
		int line = c == 'H' ? top : c == 'L' ? bottom : (top + bottom) / 2;
		cur = first_nonblank(offset_of_line(line));
		return;
	}
	case 'z': {
		int k = key(), rows = terminal_rows() - 2, line = line_of(cur);
		top = k == 't' ? line : k == 'b' ? line - rows + 1 : line - rows / 2;
		if (top < 0)
			top = 0;
		return;
	}
	case '.':
		memcpy(replay, last_change, sizeof replay);
		replay_len = last_change_len;
		replay_at = 0;
		return;
	case KEY_DELETE:
		c = 'x';
		break;
	case 'i': group++; mode = INSERT; return;
	case 'a': group++; mode = INSERT; if (cur < line_end(cur)) cur++; return;
	case 'I': group++; mode = INSERT; cur = first_nonblank(cur); return;
	case 'A': group++; mode = INSERT; cur = line_end(cur); return;
	case 'o': open_line(1); return;
	case 'O': open_line(0); return;
	case 'v': mode = VISUAL; vstart = cur; return;
	case 'V': mode = VLINE; vstart = cur; return;
	case ':': start_line_input(COMMAND); return;
	case '/': start_line_input(SEARCH); return;
	case 'u': undo(); cur = clamp_normal(cur); return;
	case 18: redo(); cur = clamp_normal(cur); return;
	case 'p': paste(0, count); return;
	case 'P': paste(1, count); return;
	}
	switch (c) {
	case 'x':
		if (cur < line_end(cur)) {
			group++;
			int e = cur + count < line_end(cur) ? cur + count : line_end(cur);
			set_yank(cur, e, 0);
			erase(cur, e - cur);
		}
		cur = clamp_normal(cur);
		return;
	case 'X':
		if (cur > line_start(cur)) {
			group++;
			int s = cur - count > line_start(cur) ? cur - count : line_start(cur);
			set_yank(s, cur, 0);
			erase(s, cur - s);
			cur = s;
		}
		return;
	case 'r': {
		int ch = key();
		if (ch >= ' ' && ch < 127 && cur + count <= line_end(cur)) {
			group++;
			char buf[64];
			int n = count < 64 ? count : 64;
			memset(buf, ch, n);
			erase(cur, n);
			insert(cur, buf, n);
			cur += n - 1;
		}
		return;
	}
	case '~':
		group++;
		for (int i = 0; i < count && cur < line_end(cur); i++) {
			char ch = text[cur];
			if (ch >= 'a' && ch <= 'z')
				ch -= 32;
			else if (ch >= 'A' && ch <= 'Z')
				ch += 32;
			erase(cur, 1);
			insert(cur, &ch, 1);
			cur++;
		}
		cur = clamp_normal(cur);
		return;
	case 'J':
		group++;
		for (int i = 0; i < (count > 1 ? count - 1 : 1); i++) {
			int e = line_end(cur);
			if (e >= len)
				break;
			int n = 1;
			while (e + n < len && (text[e + n] == ' ' || text[e + n] == '\t'))
				n++;
			erase(e, n);
			insert(e, " ", 1);
			cur = e;
		}
		return;
	case 'D': apply('d', cur, line_end(cur), 0); cur = clamp_normal(cur); return;
	case 'C': apply('c', cur, line_end(cur), 0); return;
	case 's': apply('c', cur, cur + count < line_end(cur) ? cur + count : line_end(cur), 0); return;
	case 'S': apply('c', cur, cur, 1); return;
	case 'Z': {
		int k = key();
		if (k == 'Z')
			ex("x");
		else if (k == 'Q')
			quit_now = 1;
		return;
	}
	case 'd': case 'c': case 'y': case '>': case '<': {
		int k = key();
		int kc = 0;
		while ((k >= '1' && k <= '9') || (kc && k == '0')) {
			kc = kc * 10 + k - '0';
			k = key();
		}
		if (kc)
			count *= kc;
		if (k == c) {
			int last = line_of(cur) + count - 1;
			if (last >= line_count())
				last = line_count() - 1;
			apply(c, cur, offset_of_line(last), 1);
			if (c != 'c')
				cur = clamp_normal(first_nonblank(cur));
			return;
		}
		if (k == 'i' || k == 'a') {
			int s, e;
			if (text_object(k == 'a', key(), &s, &e) == 0)
				apply(c, s, e, 0);
			return;
		}
		if (c == 'c' && (k == 'w' || k == 'W')) {
			int e = cur;
			for (int i = 0; i < count; i++)
				e = word_end(e, k == 'W') + 1;
			apply('c', cur, e, 0);
			return;
		}
		int lw, inc, ok;
		if (k == 'G' && had_count)
			want_col = -2;
		int to = motion(k, count, &lw, &inc, &ok);
		want_col = -1;
		if (!ok)
			return;
		if (lw)
			apply(c, cur, to, 1);
		else
			apply(c, cur, to > cur ? to + inc : to, 0);
		if (c != 'c')
			cur = clamp_normal(cur);
		return;
	}
	}
	int lw, inc, ok;
	if (c == 'G' && had_count)
		want_col = -2;
	int to = motion(c, count, &lw, &inc, &ok);
	int vertical = c == 'j' || c == 'k' || c == 0x100 || c == 0x101 || c == 4 || c == 21 || c == 6 || c == 2;
	if (!vertical)
		want_col = -1;
	else if (want_col < 0)
		want_col = column(cur);
	if (ok)
		cur = clamp_normal(to);
}

int editor_main(const char *p)
{
	path[0] = 0;
	len = cur = top = left = 0;
	mode = NORMAL;
	message[0] = 0;
	highlight_search = 0;
	if (p && load(p) < 0) {
		sh_error("%s", message);
		return -1;
	}
	quit_now = quit_asked = 0;
	replay_len = replay_at = 0;
	char hint[160];
	snprintf(hint, sizeof hint, "%s%sctrl+s save   ctrl+q quit   i to type, esc to stop", message, message[0] ? "   " : "");
	strcpy(message, hint);
	terminal_raw(1);
	while (!quit_now) {
		draw();
		int c = key();
		// work in any mode, not everyone knows vi
		if (c == 17 || c == 19) {
			if (recording && rec_len)
				rec_len--;
			if (c == 19) {
				message_error = 0;
				save(NULL);
			} else if (modified && !quit_asked) {
				quit_asked = 1;
				message_error = 1, snprintf(message, sizeof message, "not saved. ctrl+q again to quit anyway, ctrl+s to save");
			} else {
				quit_now = 1;
			}
			continue;
		}
		quit_asked = 0;
		if (mode == NORMAL && c != '.' && !from_replay) {
			recording = 1;
			rec_len = 0;
			rec[rec_len++] = c;
			rec_from = changes;
		}
		if (mode == INSERT)
			insert_key(c);
		else if (mode == COMMAND || mode == SEARCH)
			line_input_key(c);
		else
			normal_key(c);
		if (!recording)
			continue;
		if (mode == COMMAND || mode == SEARCH || mode == VISUAL || mode == VLINE) {
			recording = 0;
		} else if (mode == NORMAL) {
			if (changes != rec_from) {
				memcpy(last_change, rec, sizeof rec);
				last_change_len = rec_len;
			}
			recording = 0;
		}
	}
	recording = 0;
	terminal_raw(0);
	terminal_color(WHITE);
	return 0;
}
