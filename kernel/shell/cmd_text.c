#include "kernel.h"
#include "ppc.h"
#include "screen.h"
#include "shell.h"

// the file or the whole pipe goes into buf and gets cut into lines in place
#define MAX_LINES 32768

static char buf[FS_FILE_MAX + 1], buf2[FS_FILE_MAX + 1];
static char *lines[MAX_LINES], *lines2[MAX_LINES];

const struct command *sh_lookup(const char *name);

static int flag(int *argc, char **argv, const char *f)
{
	for (int i = 1; i < *argc; i++)
		if (!strcmp(argv[i], f)) {
			for (int k = i; k < *argc - 1; k++)
				argv[k] = argv[k + 1];
			(*argc)--;
			return 1;
		}
	return 0;
}

static int option(int *argc, char **argv, const char *f, long *value)
{
	for (int i = 1; i < *argc - 1; i++)
		if (!strcmp(argv[i], f)) {
			if (sh_number(argv[i + 1], value))
				return -1;
			for (int k = i; k < *argc - 2; k++)
				argv[k] = argv[k + 2];
			*argc -= 2;
			return 1;
		}
	return 0;
}

static const char *option_text(int *argc, char **argv, const char *f)
{
	for (int i = 1; i < *argc - 1; i++)
		if (!strcmp(argv[i], f)) {
			const char *v = argv[i + 1];
			for (int k = i; k < *argc - 2; k++)
				argv[k] = argv[k + 2];
			*argc -= 2;
			return v;
		}
	return NULL;
}

static int split(char *text, int len, char **out)
{
	int n = 0;
	text[len] = 0;
	for (char *s = text; *s && n < MAX_LINES;) {
		out[n++] = s;
		char *e = strchr(s, '\n');
		if (!e)
			break;
		*e = 0;
		s = e + 1;
	}
	return n;
}

static int load(int argc, char **argv, int first, char **out)
{
	int len = sh_input(argc, argv, first, buf, FS_FILE_MAX);
	if (len < 0)
		return -1;
	return split(buf, len, out);
}

static void cmd_head(int argc, char **argv)
{
	long n = 10;
	if (option(&argc, argv, "-n", &n) < 0)
		return;
	int count = load(argc, argv, 1, lines);
	for (int i = 0; i < count && i < n; i++)
		out("%s\n", lines[i]);
}

static void cmd_tail(int argc, char **argv)
{
	long n = 10;
	if (option(&argc, argv, "-n", &n) < 0)
		return;
	int count = load(argc, argv, 1, lines);
	for (int i = count > n ? count - n : 0; i < count; i++)
		out("%s\n", lines[i]);
}

static char lower(char c)
{
	return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static int contains(const char *hay, const char *needle, int nocase)
{
	if (!nocase)
		return strstr(hay, needle) != NULL;
	int nl = strlen(needle);
	for (; *hay; hay++) {
		int i = 0;
		while (i < nl && hay[i] && lower(hay[i]) == lower(needle[i]))
			i++;
		if (i == nl)
			return 1;
	}
	return !nl;
}

static void cmd_grep(int argc, char **argv)
{
	int invert = flag(&argc, argv, "-v"), nocase = flag(&argc, argv, "-i");
	int number = flag(&argc, argv, "-n"), count_only = flag(&argc, argv, "-c");
	if (argc < 2) {
		sh_error("Usage: grep [-v] [-i] [-n] [-c] <text> [file...]");
		return;
	}
	int n = load(argc, argv, 2, lines), hits = 0;
	for (int i = 0; i < n; i++)
		if (contains(lines[i], argv[1], nocase) != invert) {
			hits++;
			if (count_only)
				continue;
			if (number)
				out("%d:", i + 1);
			out("%s\n", lines[i]);
		}
	if (count_only)
		out("%d\n", hits);
}

static int numeric;

static long leading_number(const char *s)
{
	long v = 0;
	int neg = 0;
	while (*s == ' ')
		s++;
	if (*s == '-')
		neg = 1, s++;
	while (*s >= '0' && *s <= '9')
		v = v * 10 + *s++ - '0';
	return neg ? -v : v;
}

static int compare(const char *a, const char *b)
{
	if (numeric) {
		long x = leading_number(a), y = leading_number(b);
		return x < y ? -1 : x > y;
	}
	return strcmp(a, b);
}

// merge sort, stable so equal numbers keep their order with -n
static void sort_lines(char **v, char **tmp, int n)
{
	if (n < 2)
		return;
	int h = n / 2;
	sort_lines(v, tmp, h);
	sort_lines(v + h, tmp, n - h);
	int i = 0, j = h, k = 0;
	while (i < h && j < n)
		tmp[k++] = compare(v[j], v[i]) < 0 ? v[j++] : v[i++];
	while (i < h)
		tmp[k++] = v[i++];
	while (j < n)
		tmp[k++] = v[j++];
	memcpy(v, tmp, sizeof *v * n);
}

static void cmd_sort(int argc, char **argv)
{
	int reverse = flag(&argc, argv, "-r"), unique = flag(&argc, argv, "-u");
	numeric = flag(&argc, argv, "-n");
	int n = load(argc, argv, 1, lines);
	if (n < 0)
		return;
	sort_lines(lines, lines2, n);
	for (int k = 0; k < n; k++) {
		int i = reverse ? n - 1 - k : k;
		if (unique && k && !strcmp(lines[i], lines[reverse ? i + 1 : i - 1]))
			continue;
		out("%s\n", lines[i]);
	}
}

static void cmd_uniq(int argc, char **argv)
{
	int count = flag(&argc, argv, "-c"), dups = flag(&argc, argv, "-d");
	int n = load(argc, argv, 1, lines);
	for (int i = 0; i < n;) {
		int j = i + 1;
		while (j < n && !strcmp(lines[i], lines[j]))
			j++;
		if (dups && j - i < 2) {
			i = j;
			continue;
		}
		if (count)
			out("%7d %s\n", j - i, lines[i]);
		else
			out("%s\n", lines[i]);
		i = j;
	}
}

static int wanted(const char *list, int f)
{
	while (*list) {
		long a = 0, b;
		while (*list >= '0' && *list <= '9')
			a = a * 10 + *list++ - '0';
		b = a;
		if (*list == '-') {
			list++;
			b = 0;
			while (*list >= '0' && *list <= '9')
				b = b * 10 + *list++ - '0';
			if (!b)
				b = 1 << 30;
		}
		if (f >= a && f <= b)
			return 1;
		if (*list == ',')
			list++;
		else if (*list)
			return 0;
	}
	return 0;
}

static void cmd_cut(int argc, char **argv)
{
	const char *delim = option_text(&argc, argv, "-d");
	const char *fields = option_text(&argc, argv, "-f");
	const char *chars = option_text(&argc, argv, "-c");
	if (!fields && !chars) {
		sh_error("Usage: cut -f <list> [-d <c>] | -c <list> [file...]");
		return;
	}
	char d = delim && *delim ? *delim : '\t';
	int n = load(argc, argv, 1, lines);
	for (int i = 0; i < n; i++) {
		if (chars) {
			for (int c = 0; lines[i][c]; c++)
				if (wanted(chars, c + 1))
					out("%c", lines[i][c]);
			out("\n");
			continue;
		}
		int f = 1, first = 1;
		const char *s = lines[i];
		while (1) {
			const char *e = strchr(s, d);
			int len = e ? e - s : (int)strlen(s);
			if (wanted(fields, f)) {
				if (!first)
					out("%c", d);
				out_write(s, len);
				first = 0;
			}
			if (!e)
				break;
			s = e + 1;
			f++;
		}
		out("\n");
	}
}

static void cmd_rev(int argc, char **argv)
{
	int n = load(argc, argv, 1, lines);
	for (int i = 0; i < n; i++) {
		for (int k = strlen(lines[i]) - 1; k >= 0; k--)
			out_write(&lines[i][k], 1);
		out("\n");
	}
}

static void cmd_tac(int argc, char **argv)
{
	int n = load(argc, argv, 1, lines);
	for (int i = n - 1; i >= 0; i--)
		out("%s\n", lines[i]);
}

static void cmd_nl(int argc, char **argv)
{
	int n = load(argc, argv, 1, lines), k = 0;
	for (int i = 0; i < n; i++)
		if (lines[i][0])
			out("%6d\t%s\n", ++k, lines[i]);
		else
			out("\n");
}

static void cmd_wc(int argc, char **argv)
{
	int only_l = flag(&argc, argv, "-l"), only_w = flag(&argc, argv, "-w"), only_c = flag(&argc, argv, "-c");
	int all = !only_l && !only_w && !only_c;
	int files = argc > 1 ? argc - 1 : 1;
	long tl = 0, tw = 0, tc = 0;
	for (int f = 0; f < files; f++) {
		int len;
		if (argc > 1) {
			len = fs_read(argv[1 + f], buf, FS_FILE_MAX);
			if (len < 0) {
				sh_fs_error(argv[1 + f], len);
				continue;
			}
		} else {
			len = sh_input(argc, argv, 1, buf, FS_FILE_MAX);
			if (len < 0)
				return;
		}
		long l = 0, w = 0, in = 0;
		for (int i = 0; i < len; i++) {
			l += buf[i] == '\n';
			int space = buf[i] == ' ' || buf[i] == '\n' || buf[i] == '\t' || buf[i] == '\r';
			w += !space && !in;
			in = !space;
		}
		tl += l, tw += w, tc += len;
		if (all || only_l)
			out("%7ld", l);
		if (all || only_w)
			out(" %7ld", w);
		if (all || only_c)
			out(" %7d", len);
		out(argc > 1 ? " %s\n" : "\n", argc > 1 ? argv[1 + f] : "");
	}
	if (files > 1)
		out("%7ld %7ld %7ld total\n", tl, tw, tc);
}

static void cmd_cmp(int argc, char **argv)
{
	if (argc != 3) {
		sh_error("Usage: cmp <a> <b>");
		return;
	}
	int a = fs_read(argv[1], buf, FS_FILE_MAX), b = fs_read(argv[2], buf2, FS_FILE_MAX);
	if (a < 0 || b < 0) {
		sh_fs_error(a < 0 ? argv[1] : argv[2], a < 0 ? a : b);
		return;
	}
	int line = 1;
	for (int i = 0; i < a && i < b; i++) {
		if (buf[i] != buf2[i]) {
			out("%s %s differ: byte %d, line %d\n", argv[1], argv[2], i + 1, line);
			return;
		}
		line += buf[i] == '\n';
	}
	if (a != b)
		out("cmp: EOF on %s\n", a < b ? argv[1] : argv[2]);
}

// FIXME lcs table on up to 600 lines a side, anything bigger is refused. needs a
// real myers diff one day
static uint16_t lcs[601][601];

static void cmd_diff(int argc, char **argv)
{
	if (argc != 3) {
		sh_error("Usage: diff <a> <b>");
		return;
	}
	int la = fs_read(argv[1], buf, FS_FILE_MAX), lb = fs_read(argv[2], buf2, FS_FILE_MAX);
	if (la < 0 || lb < 0) {
		sh_fs_error(la < 0 ? argv[1] : argv[2], la < 0 ? la : lb);
		return;
	}
	int n = split(buf, la, lines), m = split(buf2, lb, lines2);
	if (n > 600 || m > 600) {
		sh_error("diff: files over 600 lines are too big for now");
		return;
	}
	for (int i = n; i >= 0; i--)
		for (int j = m; j >= 0; j--)
			lcs[i][j] = i == n || j == m ? 0 : !strcmp(lines[i], lines2[j]) ? lcs[i + 1][j + 1] + 1
				: lcs[i + 1][j] > lcs[i][j + 1] ? lcs[i + 1][j] : lcs[i][j + 1];
	int i = 0, j = 0;
	while (i < n || j < m) {
		if (i < n && j < m && !strcmp(lines[i], lines2[j])) {
			i++, j++;
		} else if (j < m && (i == n || lcs[i][j + 1] >= lcs[i + 1][j])) {
			out_color(LIGHT_GREEN);
			out("+ %s\n", lines2[j++]);
		} else {
			out_color(LIGHT_RED);
			out("- %s\n", lines[i++]);
		}
	}
	out_color(WHITE);
}

// more and less are the same, 17 lines and wait for a key. in a pipe it just prints
static void cmd_less(int argc, char **argv)
{
	int n = load(argc, argv, 1, lines);
	const char *dummy;
	int piped = sh_stdin(&dummy) >= 0;
	int left = 17;
	for (int i = 0; i < n; i++) {
		out("%s\n", lines[i]);
		if (piped || --left > 0 || i + 1 == n)
			continue;
		out_color(YELLOW);
		out("-- more --  space next page, enter one line, q quit");
		out_color(WHITE);
		int c = sh_read_key();
		out("\r                                                    \r");
		if (c == 'q')
			return;
		left = c == '\n' || c == '\r' ? 1 : 17;
	}
}

static int range(const char *set, char *out, int room)
{
	int n = 0;
	for (int i = 0; set[i] && n < room; i++) {
		if (set[i + 1] == '-' && set[i + 2]) {
			for (int c = (unsigned char)set[i]; c <= (unsigned char)set[i + 2] && n < room; c++)
				out[n++] = c;
			i += 2;
		} else {
			out[n++] = set[i];
		}
	}
	return n;
}

static void cmd_tr(int argc, char **argv)
{
	int del = flag(&argc, argv, "-d");
	if (argc < 2 || (!del && argc < 3)) {
		sh_error("Usage: tr <from> <to>  or  tr -d <chars>");
		return;
	}
	const char *in;
	int len = sh_stdin(&in);
	if (len < 0) {
		sh_error("tr: reads a pipe");
		return;
	}
	char from[256], to[256];
	int nf = range(argv[1], from, 256), nt = del ? 0 : range(argv[2], to, 256);
	for (int i = 0; i < len; i++) {
		char c = in[i];
		int k = 0;
		while (k < nf && from[k] != c)
			k++;
		if (k < nf) {
			if (del)
				continue;
			c = to[k < nt ? k : nt - 1];
		}
		out_write(&c, 1);
	}
}

static void cmd_tee(int argc, char **argv)
{
	int append = flag(&argc, argv, "-a");
	const char *in;
	int len = sh_stdin(&in);
	if (len < 0) {
		sh_error("tee: reads a pipe");
		return;
	}
	for (int i = 1; i < argc; i++) {
		int r = append ? fs_append(argv[i], in, len) : fs_write(argv[i], in, len);
		if (r < 0)
			sh_fs_error(argv[i], r);
	}
	out_write(in, len);
}

static void cmd_paste(int argc, char **argv)
{
	if (argc < 2 || argc > 9) {
		sh_error("Usage: paste <file>...   (up to 8)");
		return;
	}
	static char *cols[8][1024];
	int counts[8], most = 0, used = 0;
	for (int f = 0; f < argc - 1; f++) {
		int l = fs_read(argv[f + 1], buf + used, FS_FILE_MAX - used - 1);
		if (l < 0) {
			sh_fs_error(argv[f + 1], l);
			return;
		}
		char *base = buf + used;
		used += l + 1;
		counts[f] = 0;
		base[l] = 0;
		for (char *s = base; *s && counts[f] < 1024;) {
			cols[f][counts[f]++] = s;
			char *e = strchr(s, '\n');
			if (!e)
				break;
			*e = 0;
			s = e + 1;
		}
		if (counts[f] > most)
			most = counts[f];
	}
	for (int i = 0; i < most; i++)
		for (int f = 0; f < argc - 1; f++)
			out("%s%s", i < counts[f] ? cols[f][i] : "", f == argc - 2 ? "\n" : "\t");
}

static void cmd_comm(int argc, char **argv)
{
	int hide[4] = {0};
	while (argc > 1 && argv[1][0] == '-' && argv[1][1]) {
		for (const char *f = argv[1] + 1; *f; f++)
			if (*f >= '1' && *f <= '3')
				hide[*f - '0'] = 1;
		for (int k = 1; k < argc - 1; k++)
			argv[k] = argv[k + 1];
		argc--;
	}
	if (argc != 3) {
		sh_error("Usage: comm [-123] <a> <b>   (both sorted)");
		return;
	}
	int la = fs_read(argv[1], buf, FS_FILE_MAX), lb = fs_read(argv[2], buf2, FS_FILE_MAX);
	if (la < 0 || lb < 0) {
		sh_fs_error(la < 0 ? argv[1] : argv[2], la < 0 ? la : lb);
		return;
	}
	int n = split(buf, la, lines), m = split(buf2, lb, lines2), i = 0, j = 0;
	while (i < n || j < m) {
		int c = i == n ? 1 : j == m ? -1 : strcmp(lines[i], lines2[j]);
		int column = c < 0 ? 1 : c > 0 ? 2 : 3;
		const char *text = c > 0 ? lines2[j] : lines[i];
		if (c <= 0)
			i++;
		if (c >= 0)
			j++;
		if (hide[column])
			continue;
		for (int k = 1; k < column; k++)
			if (!hide[k])
				out("\t");
		out("%s\n", text);
	}
}

static void cmd_fold(int argc, char **argv)
{
	long w = 80;
	if (option(&argc, argv, "-w", &w) < 0 || w < 1)
		return;
	int n = load(argc, argv, 1, lines);
	for (int i = 0; i < n; i++) {
		int len = strlen(lines[i]);
		for (int at = 0; at < len || at == 0; at += w) {
			out_write(lines[i] + at, len - at < w ? len - at : w);
			out("\n");
			if (!len)
				break;
		}
	}
}

static void cmd_strings(int argc, char **argv)
{
	long min = 4;
	if (option(&argc, argv, "-n", &min) < 0 || min < 1)
		return;
	int len = sh_input(argc, argv, 1, buf, FS_FILE_MAX);
	for (int i = 0; i < len;) {
		int j = i;
		while (j < len && buf[j] >= 32 && buf[j] < 127)
			j++;
		if (j - i >= min) {
			out_write(buf + i, j - i);
			out("\n");
		}
		i = j + 1;
	}
}

static void cmd_xxd(int argc, char **argv)
{
	int len = sh_input(argc, argv, 1, buf, FS_FILE_MAX);
	for (int at = 0; at < len; at += 16) {
		out("%08x: ", at);
		for (int i = 0; i < 16; i++) {
			if (at + i < len)
				out("%02x", (unsigned char)buf[at + i]);
			else
				out("  ");
			if (i & 1)
				out(" ");
		}
		out(" ");
		for (int i = 0; i < 16 && at + i < len; i++) {
			char c = buf[at + i];
			out("%c", c >= 32 && c < 127 ? c : '.');
		}
		out("\n");
	}
}

// this is the zip crc32, not the one posix cksum prints, so the numbers wont match linux
// cksum, they match crc32 from zlib
static uint32_t crc32(const uint8_t *p, int n)
{
	uint32_t c = 0xffffffff;
	for (int i = 0; i < n; i++) {
		c ^= p[i];
		for (int k = 0; k < 8; k++)
			c = c >> 1 ^ (0xedb88320 & -(c & 1));
	}
	return ~c;
}

static void cmd_cksum(int argc, char **argv)
{
	if (argc < 2) {
		int len = sh_input(argc, argv, 1, buf, FS_FILE_MAX);
		if (len >= 0)
			out("%u %d\n", crc32((uint8_t *)buf, len), len);
		return;
	}
	for (int i = 1; i < argc; i++) {
		int len = fs_read(argv[i], buf, FS_FILE_MAX);
		if (len < 0)
			sh_fs_error(argv[i], len);
		else
			out("%u %d %s\n", crc32((uint8_t *)buf, len), len, argv[i]);
	}
}

// one run with every word from the pipe added, not one run per line
static void cmd_xargs(int argc, char **argv)
{
	const char *in;
	int len = sh_stdin(&in);
	if (argc < 2 || len < 0) {
		sh_error("Usage: ... | xargs <command> [args]");
		return;
	}
	const struct command *c = sh_lookup(argv[1]);
	if (!c) {
		sh_error("xargs: %s: Unknown command", argv[1]);
		return;
	}
	static char words[SH_BUF];
	char *v[SH_ARGS + 1];
	int n = 0;
	for (int i = 1; i < argc && n < SH_ARGS; i++)
		v[n++] = argv[i];
	memcpy(words, in, len < SH_BUF - 1 ? len : SH_BUF - 1);
	words[len < SH_BUF - 1 ? len : SH_BUF - 1] = 0;
	for (char *s = words; *s && n < SH_ARGS;) {
		while (*s == ' ' || *s == '\n' || *s == '\t')
			*s++ = 0;
		if (!*s)
			break;
		v[n++] = s;
		while (*s && *s != ' ' && *s != '\n' && *s != '\t')
			s++;
	}
	v[n] = NULL;
	c->fn(n, v);
}

const struct command cmd_text[] = {
	{"head", cmd_head}, {"tail", cmd_tail}, {"grep", cmd_grep}, {"sort", cmd_sort}, {"uniq", cmd_uniq},
	{"cut", cmd_cut}, {"rev", cmd_rev}, {"tac", cmd_tac}, {"nl", cmd_nl}, {"wc", cmd_wc}, {"cmp", cmd_cmp},
	{"diff", cmd_diff}, {"less", cmd_less}, {"more", cmd_less}, {"tr", cmd_tr}, {"tee", cmd_tee},
	{"paste", cmd_paste}, {"comm", cmd_comm}, {"fold", cmd_fold}, {"strings", cmd_strings},
	{"xxd", cmd_xxd}, {"cksum", cmd_cksum}, {"xargs", cmd_xargs},
	{NULL, NULL}
};
