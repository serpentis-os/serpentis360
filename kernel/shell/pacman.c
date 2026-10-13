#include "kernel.h"
#include "ppc.h"
#include "screen.h"
#include "shell.h"
#include "usb.h"
#include "keys.h"

// pacman. a tile is two cells, the sprites are in the font so they come out round.
// the best score goes in ~/.pacman

static int ox, oy;
static uint64_t seed;
static const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, -1, 1};	// right left up down

static unsigned rnd(unsigned n)
{
	if (!seed)
		seed = mftb() | 1;
	seed ^= seed << 13;
	seed ^= seed >> 7;
	seed ^= seed << 17;
	return seed % n;
}

static void tile(int x, int y, int spr, int fg)
{
	terminal_cell(ox + x * 2, oy + y, SPRITE(spr), fg, BLACK);
	terminal_cell(ox + x * 2 + 1, oy + y, SPRITE(spr) + 1, fg, BLACK);
}

static void empty(int x, int y)
{
	terminal_cell(ox + x * 2, oy + y, ' ', WHITE, BLACK);
	terminal_cell(ox + x * 2 + 1, oy + y, ' ', WHITE, BLACK);
}

static void text(int c, int r, const char *s, int fg)
{
	for (; *s; s++, c++)
		terminal_cell(c, r, *s, fg, BLACK);
}

static void centered(int r, const char *s, int fg)
{
	text((terminal_cols() - (int)strlen(s)) / 2, r, s, fg);
}

static void status(const char *s)
{
	int c = 0;
	for (; s[c] && c < terminal_cols(); c++)
		terminal_cell(c, 0, s[c], WHITE, BLACK);
	for (; c < terminal_cols(); c++)
		terminal_cell(c, 0, ' ', WHITE, BLACK);
}

static void clear_all(void)
{
	for (int r = 0; r < terminal_rows(); r++)
		for (int c = 0; c < terminal_cols(); c++)
			terminal_cell(c, r, ' ', WHITE, BLACK);
}

static int direction(int k)
{
	k &= ~KEY_MODS;
	if (k == KEY_RIGHT || k == 'd')
		return 0;
	if (k == KEY_LEFT || k == 'a')
		return 1;
	if (k == KEY_UP || k == 'w')
		return 2;
	if (k == KEY_DOWN || k == 's')
		return 3;
	return -1;
}

static int quit_key(int k)
{
	return k == KEY_ESC || k == 'q' || k == KEY_INTERRUPT;
}

static void drain(void)
{
	while (input_getc() >= 0)
		;
}

static int again(const char *title, int score, int best)
{
	char line[64];
	int w = 36, r = terminal_rows() / 2 - 3, c = (terminal_cols() - w) / 2;
	for (int y = r; y < r + 7; y++)
		for (int x = c; x < c + w; x++) {
			int edge = y == r || y == r + 6 || x == c || x == c + w - 1;
			terminal_cell(x, y, ' ', WHITE, edge ? BLUE : BLACK);
		}
	centered(r + 2, title, LIGHT_RED);
	snprintf(line, sizeof line, "score %d   best %d", score, best);
	centered(r + 3, line, WHITE);
	centered(r + 4, "enter: again    esc: quit", YELLOW);
	sleep_ms(500);
	drain();
	for (;;) {
		int k = sh_read_key();
		if (k == '\n' || k == ' ' || k == 'y')
			return 1;
		if (quit_key(k) || k == 'n')
			return 0;
	}
}

static int load_best(void)
{
	char path[FS_PATH_MAX], buf[16];
	long v;
	snprintf(path, sizeof path, "%s/.pacman", users_current()->home);
	int n = fs_device() >= 0 ? fs_read(path, buf, sizeof buf - 1) : -1;
	if (n <= 0)
		return 0;
	buf[n] = 0;
	while (n && (buf[n - 1] == '\n' || buf[n - 1] == ' '))
		buf[--n] = 0;
	return sh_number(buf, &v) ? 0 : (int)v;
}

static void save_best(int best)
{
	char path[FS_PATH_MAX], buf[16];
	if (fs_device() < 0)
		return;
	snprintf(path, sizeof path, "%s/.pacman", users_current()->home);
	int n = snprintf(buf, sizeof buf, "%d\n", best);
	fs_write(path, buf, n);
}

static void enter_game(void)
{
	terminal_raw(1);
	terminal_cursor(0);
	drain();
}

static void leave_game(void)
{
	terminal_cursor(1);
	terminal_raw(0);
}

#define MW 28
#define MH 31

static const char *const maze_src[MH] = {
	"############################",
	"#............##............#",
	"#.####.#####.##.#####.####.#",
	"#o####.#####.##.#####.####o#",
	"#.####.#####.##.#####.####.#",
	"#..........................#",
	"#.####.##.########.##.####.#",
	"#.####.##.########.##.####.#",
	"#......##....##....##......#",
	"######.##### ## #####.######",
	"     #.##### ## #####.#     ",
	"     #.##          ##.#     ",
	"     #.## ###--### ##.#     ",
	"######.## #      # ##.######",
	"      .   #      #   .      ",
	"######.## #      # ##.######",
	"     #.## ######## ##.#     ",
	"     #.##          ##.#     ",
	"     #.## ######## ##.#     ",
	"######.## ######## ##.######",
	"#............##............#",
	"#.####.#####.##.#####.####.#",
	"#.####.#####.##.#####.####.#",
	"#o..##.......  .......##..o#",
	"###.##.##.########.##.##.###",
	"###.##.##.########.##.##.###",
	"#......##....##....##......#",
	"#.##########.##.##########.#",
	"#.##########.##.##########.#",
	"#..........................#",
	"############################",
};

struct ghost {
	int x, y, dir;
	int color, out, wait, scared;
	int start_x;
};

static char maze[MH][MW];
static struct ghost ghosts[4];
static int px, py, pdir, pwant, mouth, dots, fright, wall_color = BLUE;

static int wrap(int x)
{
	return x < 0 ? MW - 1 : x >= MW ? 0 : x;
}

static int open_tile(int x, int y)
{
	return maze[y][wrap(x)] != '#' && maze[y][wrap(x)] != '-';
}

// pacman on top, then a ghost, then the maze
static void paint(int x, int y)
{
	if (x == px && y == py) {
		static const int open_mouth[4] = {SPR_PAC_R, SPR_PAC_L, SPR_PAC_U, SPR_PAC_D};
		tile(x, y, mouth ? open_mouth[pdir] : SPR_PAC, YELLOW);
		return;
	}
	for (int i = 0; i < 4; i++) {
		struct ghost *g = &ghosts[i];
		if (g->x != x || g->y != y)
			continue;
		if (g->scared)
			tile(x, y, SPR_SCARED, fright < 12 && fright / 2 % 2 ? WHITE : LIGHT_BLUE);
		else
			tile(x, y, SPR_GHOST, g->color);
		return;
	}
	switch (maze[y][x]) {
	case '#':
		tile(x, y, SPR_WALL, wall_color);
		break;
	case '.':
		tile(x, y, SPR_DOT, LIGHT_GRAY);
		break;
	case 'o':
		tile(x, y, SPR_PELLET, WHITE);
		break;
	case '-':
		terminal_cell(ox + x * 2, oy + y, '-', LIGHT_MAGENTA, BLACK);
		terminal_cell(ox + x * 2 + 1, oy + y, '-', LIGHT_MAGENTA, BLACK);
		break;
	default:
		empty(x, y);
	}
}

static void paint_all(void)
{
	for (int y = 0; y < MH; y++)
		for (int x = 0; x < MW; x++)
			paint(x, y);
}

// red starts outside, the others wait in the house and come out one after the other
static void reset_actors(void)
{
	static const int colors[4] = {LIGHT_RED, LIGHT_MAGENTA, LIGHT_CYAN, BROWN};
	static const int house_x[4] = {13, 13, 11, 15}, waits[4] = {0, 30, 80, 130};
	px = 13;
	py = 23;
	pdir = pwant = 1;
	mouth = 0;
	fright = 0;
	for (int i = 0; i < 4; i++) {
		struct ghost *g = &ghosts[i];
		g->color = colors[i];
		g->start_x = house_x[i];
		g->x = house_x[i];
		g->y = i ? 14 : 11;
		g->out = i == 0;
		g->wait = waits[i];
		g->dir = 1;
		g->scared = 0;
	}
}

static void ghost_home(struct ghost *g, int wait)
{
	g->x = g->start_x;
	g->y = 14;
	g->out = 0;
	g->scared = 0;
	g->wait = wait;
}

// the arcade rules. red chases, pink aims 4 ahead, cyan doubles the line from red to 2
// ahead of you, orange chases until it gets close and then runs to its corner
static void target(int i, int scatter, int *tx, int *ty)
{
	static const int corner_x[4] = {25, 2, 27, 0}, corner_y[4] = {-3, -3, 31, 31};
	struct ghost *g = &ghosts[i];
	int ddx = g->x - px, ddy = g->y - py;
	if (scatter || (i == 3 && ddx * ddx + ddy * ddy < 64)) {
		*tx = corner_x[i];
		*ty = corner_y[i];
	} else if (i == 1) {
		*tx = px + 4 * dx[pdir];
		*ty = py + 4 * dy[pdir];
	} else if (i == 2) {
		*tx = 2 * (px + 2 * dx[pdir]) - ghosts[0].x;
		*ty = 2 * (py + 2 * dy[pdir]) - ghosts[0].y;
	} else {
		*tx = px;
		*ty = py;
	}
}

// at a junction take the way that ends closest to the target, never turn around
static void ghost_step(int i, int scatter)
{
	static const int order[4] = {2, 1, 3, 0};
	struct ghost *g = &ghosts[i];
	int tx, ty, pick = -1;
	long best = 0;
	int choices[4], n = 0;
	target(i, scatter, &tx, &ty);
	for (int k = 0; k < 4; k++) {
		int d = order[k];
		if (d == (g->dir ^ 1) || !open_tile(g->x + dx[d], g->y + dy[d]))
			continue;
		choices[n++] = d;
		long ex = wrap(g->x + dx[d]) - tx, ey = g->y + dy[d] - ty;
		if (pick < 0 || ex * ex + ey * ey < best) {
			pick = d;
			best = ex * ex + ey * ey;
		}
	}
	if (!n)
		pick = g->dir ^ 1;
	else if (g->scared)
		pick = choices[rnd(n)];
	g->dir = pick;
	g->x = wrap(g->x + dx[pick]);
	g->y += dy[pick];
}

static int score, eat_value;

static int meet(struct ghost *g)
{
	if (!g->out)
		return 0;
	if (g->scared) {
		score += eat_value;
		eat_value *= 2;
		ghost_home(g, 25);
		return 0;
	}
	return 1;
}

static void ready(int on)
{
	if (on) {
		text(ox + 22 + 3, oy + 17, "READY!", YELLOW);
		return;
	}
	for (int x = 11; x <= 16; x++)
		paint(x, 17);
}

static void lives_row(int lives)
{
	for (int i = 0; i < 5; i++) {
		if (i < lives - 1)
			tile(1 + i, MH, SPR_PAC_L, YELLOW);
		else
			empty(1 + i, MH);
	}
	text(ox + MW * 2 - 18, oy + MH, "p pause  esc quit", DARK_GRAY);
}

static void cmd_pacman(int argc, char **argv)
{
	if (terminal_cols() < MW * 2 || terminal_rows() < MH + 2) {
		sh_error("pacman: screen too small");
		return;
	}
	ox = (terminal_cols() - MW * 2) / 2;
	oy = 1;
	int best = load_best();
	enter_game();
	for (int play = 1; play;) {
		int lives = 3, level = 1, quit = 0, paused = 0;
		score = 0;
		clear_all();
		for (int newlevel = 1; lives > 0 && !quit;) {
			if (newlevel) {
				dots = 0;
				for (int y = 0; y < MH; y++)
					for (int x = 0; x < MW; x++) {
						maze[y][x] = maze_src[y][x];
						dots += maze[y][x] == '.' || maze[y][x] == 'o';
					}
				newlevel = 0;
			}
			reset_actors();
			paint_all();
			lives_row(lives);
			ready(1);
			char line[160];
			snprintf(line, sizeof line, " PACMAN   score %d   best %d   level %d", score, best, level);
			status(line);
			sleep_ms(1500);
			drain();
			ready(0);

			int step = level < 4 ? 9 - level : 5, count = 0, mode_ms = 0, died = 0;
			int fright_len = level < 6 ? 50 - level * 6 : 14;
			uint64_t next = ticks;
			while (!died && !quit && dots > 0) {
				int k;
				while ((k = input_getc()) >= 0) {
					if (quit_key(k))
						quit = 1;
					else if (k == 'p')
						paused ^= 1;
					else if (direction(k) >= 0)
						pwant = direction(k);
				}
				snprintf(line, sizeof line, " PACMAN   score %d   best %d   level %d%s", score, best, level,
					 paused ? "   paused" : "");
				status(line);
				if (quit)
					break;
				if (paused || ticks < next) {
					sleep_ms(5);
					continue;
				}
				next = ticks + step;
				count++;

				int old_x[5], old_y[5];
				old_x[4] = px;
				old_y[4] = py;
				for (int i = 0; i < 4; i++) {
					old_x[i] = ghosts[i].x;
					old_y[i] = ghosts[i].y;
				}

				if (open_tile(px + dx[pwant], py + dy[pwant]))
					pdir = pwant;
				if (open_tile(px + dx[pdir], py + dy[pdir])) {
					px = wrap(px + dx[pdir]);
					py += dy[pdir];
				}
				mouth ^= 1;
				if (maze[py][px] == '.' || maze[py][px] == 'o') {
					if (maze[py][px] == 'o') {
						fright = fright_len;
						eat_value = 200;
						for (int i = 0; i < 4; i++)
							if (ghosts[i].out) {
								ghosts[i].scared = 1;
								ghosts[i].dir ^= 1;
							}
						score += 50;
					} else {
						score += 10;
					}
					maze[py][px] = ' ';
					dots--;
				}
				for (int i = 0; i < 4 && !died; i++)
					if (ghosts[i].x == px && ghosts[i].y == py)
						died = meet(&ghosts[i]);

				// 7 s scatter, 20 s chase, twice, then chase for good
				if (!fright)
					mode_ms += step * 10;
				int scatter = mode_ms < 7000 || (mode_ms >= 27000 && mode_ms < 34000) ||
					      (mode_ms >= 54000 && mode_ms < 59000);
				for (int i = 0; i < 4 && !died; i++) {
					struct ghost *g = &ghosts[i];
					if (!g->out) {
						if (g->wait-- <= 0) {
							g->out = 1;
							g->x = 13;
							g->y = 11;
							g->dir = 1;
						}
						continue;
					}
					if (g->scared ? count % 2 : count % 5 == 0)
						continue;
					int gx = g->x, gy = g->y;
					ghost_step(i, scatter);
					int crossed = gx == px && gy == py && g->x == old_x[4] && g->y == old_y[4];
					if ((g->x == px && g->y == py) || crossed)
						died = meet(g);
				}
				if (fright && --fright == 0)
					for (int i = 0; i < 4; i++)
						ghosts[i].scared = 0;

				for (int i = 0; i < 5; i++)
					paint(old_x[i], old_y[i]);
				for (int i = 0; i < 4; i++)
					paint(ghosts[i].x, ghosts[i].y);
				paint(px, py);
				if (fright && fright < 12)
					for (int i = 0; i < 4; i++)
						if (ghosts[i].scared)
							paint(ghosts[i].x, ghosts[i].y);
			}
			if (score > best) {
				best = score;
				save_best(best);
			}
			if (quit)
				break;
			if (died) {
				for (int i = 0; i < 8; i++) {
					mouth = 1;
					pdir = i % 4;
					paint(px, py);
					sleep_ms(110);
				}
				lives--;
				lives_row(lives);
				continue;
			}
			for (int i = 0; i < 6; i++) {
				wall_color = i % 2 ? BLUE : WHITE;
				paint_all();
				sleep_ms(250);
			}
			wall_color = BLUE;
			level++;
			newlevel = 1;
		}
		if (quit)
			break;
		play = again("game over", score, best);
	}
	leave_game();
}

const struct command cmd_games[] = {
	{"pacman", cmd_pacman},
	{NULL, NULL}
};
