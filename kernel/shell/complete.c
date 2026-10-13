#include "kernel.h"
#include "screen.h"
#include "shell.h"


// tab. the first word is a command, the rest are paths. one match goes in whole, more
// fill in what they share, and when there is nothing left to fill in the list gets printed
// TODO after a | the next word should count as a command again
#define MATCHES 128

static char names[MATCHES][FS_PATH_MAX];
static int dirs[MATCHES];

static int gather(const char *word, int first, char *dir_part, int *count)
{
	int n = 0;
	const char *slash = strrchr(word, '/');
	const char *base = slash ? slash + 1 : word;
	if (first && !slash) {
		const struct command *tables[] = {cmd_core, cmd_fs, cmd_text, cmd_user, cmd_sys, cmd_source, cmd_games};
		for (size_t t = 0; t < sizeof tables / sizeof *tables; t++)
			for (const struct command *c = tables[t]; c->name && n < MATCHES; c++)
				if (!strncmp(c->name, base, strlen(base))) {
					strcpy(names[n], c->name);
					dirs[n++] = 0;
				}
		dir_part[0] = 0;
		*count = n;
		return 0;
	}
	char dir[FS_PATH_MAX];
	if (slash) {
		int len = slash - word;
		memcpy(dir_part, word, len + 1);
		dir_part[len + 1] = 0;
		snprintf(dir, sizeof dir, "%s", len ? dir_part : "/");
	} else {
		dir_part[0] = 0;
		strcpy(dir, ".");
	}
	char name[FS_PATH_MAX];
	struct fs_stat st;
	int cursor = 0;
	while (n < MATCHES && fs_readdir(dir, &cursor, name, &st) > 0) {
		if (strncmp(name, base, strlen(base)) || (name[0] == '.' && base[0] != '.'))
			continue;
		strcpy(names[n], name);
		dirs[n++] = st.dir;
	}
	*count = n;
	return 0;
}

void sh_complete(char *buf, int len, int pos, char *add, int room, int listing)
{
	add[0] = 0;
	int start = pos;
	while (start > 0 && buf[start - 1] != ' ' && buf[start - 1] != '|')
		start--;
	int first = 1;
	for (int i = 0; i < start; i++)
		if (buf[i] != ' ' && buf[i] != '|')
			first = 0;
	for (int i = start - 1; i >= 0 && first == 0; i--)
		if (buf[i] == '|') {
			first = 1;
			for (int k = i + 1; k < start; k++)
				if (buf[k] != ' ')
					first = 0;
			break;
		}
	char word[SH_LINE], dir_part[FS_PATH_MAX];
	int wl = pos - start;
	memcpy(word, buf + start, wl);
	word[wl] = 0;
	int n;
	gather(word, first, dir_part, &n);
	if (!n)
		return;
	const char *typed = strrchr(word, '/') ? strrchr(word, '/') + 1 : word;
	int tl = strlen(typed);
	if (listing) {
		for (int i = 0; i < n; i++) {
			terminal_color(dirs[i] ? LIGHT_BLUE : WHITE);
			out("%s%s  ", names[i], dirs[i] ? "/" : "");
		}
		terminal_color(WHITE);
		out("\n");
		return;
	}
	int common = strlen(names[0]);
	for (int i = 1; i < n; i++) {
		int k = 0;
		while (k < common && names[i][k] == names[0][k])
			k++;
		common = k;
	}
	int extra = common - tl;
	if (extra < 0)
		extra = 0;
	if (extra >= room - 2)
		extra = room - 2;
	memcpy(add, names[0] + tl, extra);
	add[extra] = 0;
	if (n == 1)
		strcat(add, dirs[0] ? "/" : " ");
}
