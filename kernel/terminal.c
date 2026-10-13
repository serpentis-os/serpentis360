#include "kernel.h"
#include "ppc.h"
#include "screen.h"
#include "keys.h"

// lds document. 512 lines, cursor goes anywhere, the prompt part of a line is protected.
// the editor draws on its own grid and the document comes back after

#define COLS_MAX 160
#define ROWS_MAX 64
#define DOC_LINES 512

static int ox, oy, cols, rows;
static char doc[DOC_LINES][COLS_MAX];
static uint8_t doc_attr[DOC_LINES][COLS_MAX];
static uint8_t doc_len[DOC_LINES], doc_prot[DOC_LINES], doc_cont[DOC_LINES];
static int doc_first, doc_count = 1;
static int line, col;			// cursor in the document
static int view_top, follow = 1;
static int raw;
static int raw_x, raw_y;

static char cells[ROWS_MAX][COLS_MAX];
static uint8_t attrs[ROWS_MAX][COLS_MAX];
static char shown[ROWS_MAX][COLS_MAX];
static uint8_t shown_attrs[ROWS_MAX][COLS_MAX];
static uint8_t attr = LIGHT_GRAY;
static int cursor_on = 1, cursor_x = -1, cursor_y = -1;

int terminal_ready(void)
{
	return cols > 0;
}

int terminal_cols(void)
{
	return cols;
}

int terminal_rows(void)
{
	return rows;
}

static char *doc_line(int n)
{
	return doc[(doc_first + n) % DOC_LINES];
}

static uint8_t *doc_line_attr(int n)
{
	return doc_attr[(doc_first + n) % DOC_LINES];
}

static int slot(int n)
{
	return (doc_first + n) % DOC_LINES;
}

static void draw(int c, int r)
{
	screen_char(ox + c * CELL_W, oy + r * CELL_H, cells[r][c], attrs[r][c] & 15, attrs[r][c] >> 4);
	shown[r][c] = cells[r][c];
	shown_attrs[r][c] = attrs[r][c];
}

static void hide_cursor(void)
{
	if (cursor_x >= 0) {
		draw(cursor_x, cursor_y);
		cursor_x = cursor_y = -1;
	}
}

static void show_cursor(void)
{
	hide_cursor();
	if (!cursor_on)
		return;
	int x = raw ? raw_x : col, y = raw ? raw_y : line - view_top;
	if (x >= cols)
		x = cols - 1;
	if (y < 0 || y >= rows)
		return;
	screen_char(ox + x * CELL_W, oy + y * CELL_H, cells[y][x], BLACK, LIGHT_GRAY);
	cursor_x = x;
	cursor_y = y;
}

static void flush(void)
{
	for (int r = 0; r < rows; r++)
		for (int c = 0; c < cols; c++)
			if (cells[r][c] != shown[r][c] || attrs[r][c] != shown_attrs[r][c])
				draw(c, r);
}

static void render(void)
{
	for (int r = 0; r < rows; r++) {
		int n = view_top + r;
		if (n < doc_count) {
			memcpy(cells[r], doc_line(n), cols);
			memcpy(attrs[r], doc_line_attr(n), cols);
		} else {
			memset(cells[r], ' ', cols);
			memset(attrs[r], attr, cols);
		}
	}
	flush();
}

static void blank_line(int n)
{
	memset(doc_line(n), ' ', COLS_MAX);
	memset(doc_line_attr(n), attr, COLS_MAX);
	doc_len[slot(n)] = doc_prot[slot(n)] = doc_cont[slot(n)] = 0;
}

void terminal_init(int x, int y, int c, int r)
{
	ox = x;
	oy = y;
	cols = c < COLS_MAX ? c : COLS_MAX;
	rows = r < ROWS_MAX ? r : ROWS_MAX;
	memset(shown, 0, sizeof shown);
	terminal_clear();
}

void terminal_clear(void)
{
	if (!cols)
		return;
	uint64_t msr = irq_off();
	hide_cursor();
	doc_first = 0;
	doc_count = 1;
	line = col = view_top = 0;
	follow = 1;
	blank_line(0);
	if (!raw)
		render();
	show_cursor();
	irq_restore(msr);
}

void terminal_color(int color)
{
	attr = color & 15;
}

void terminal_cursor(int on)
{
	if (!cols)
		return;
	uint64_t msr = irq_off();
	cursor_on = on;
	show_cursor();
	irq_restore(msr);
}

// keeps the cursor line on screen and scrolls only as far as it has to
static void track(void)
{
	if (!follow)
		return;
	int top = view_top;
	if (line < top)
		top = line;
	if (line >= top + rows)
		top = line - rows + 1;
	if (top != view_top) {
		view_top = top;
		render();
	}
}

static void newline(void)
{
	col = 0;
	line++;
	if (line >= DOC_LINES) {
		doc_first = (doc_first + 1) % DOC_LINES;
		line = DOC_LINES - 1;
		view_top = view_top > 0 ? view_top - 1 : 0;
	} else if (line >= doc_count) {
		doc_count = line + 1;
	}
	blank_line(line);
	track();
}

static void put_cell(char c)
{
	doc_line(line)[col] = c;
	doc_line_attr(line)[col] = attr;
	if (col >= doc_len[slot(line)])
		doc_len[slot(line)] = col + 1;
	int r = line - view_top;
	if (!raw && r >= 0 && r < rows) {
		cells[r][col] = c;
		attrs[r][col] = attr;
		if (cells[r][col] != shown[r][col] || attrs[r][col] != shown_attrs[r][col])
			draw(col, r);
	}
}

void terminal_putc(char c)
{
	if (!cols)
		return;
	uint64_t msr = irq_off();
	hide_cursor();
	if (!follow) {
		follow = 1;
		track();
	}
	switch (c) {
	case '\r':
		col = 0;
		break;
	case '\n':
		newline();
		break;
	case '\b':
		if (col > 0) {
			col--;
		} else if (line > 0) {
			line--;
			col = cols - 1;
			track();
		}
		break;
	case '\t':
		do
			put_cell(' ');
		while (++col % 4 && col < cols);
		break;
	default:
		if (col >= cols) {
			newline();
			doc_cont[slot(line)] = 1;
		}
		put_cell(c);
		col++;
	}
	show_cursor();
	irq_restore(msr);
}

void terminal_scroll(int delta)
{
	uint64_t msr = irq_off();
	hide_cursor();
	int max = doc_count - rows > 0 ? doc_count - rows : 0;
	int top = view_top + delta;
	view_top = top < 0 ? 0 : top > max ? max : top;
	follow = line >= view_top && line < view_top + rows;
	render();
	show_cursor();
	irq_restore(msr);
}

// the editor and pacman draw straight on the cell grid, the document waits and comes
// back with render()
void terminal_raw(int on)
{
	uint64_t msr = irq_off();
	hide_cursor();
	raw = on;
	raw_x = raw_y = 0;
	if (on) {
		for (int r = 0; r < rows; r++) {
			memset(cells[r], ' ', cols);
			memset(attrs[r], LIGHT_GRAY, cols);
		}
		flush();
	} else {
		render();
	}
	show_cursor();
	irq_restore(msr);
}

void terminal_cell(int c, int r, char ch, int fg, int bg)
{
	if (c < 0 || r < 0 || c >= cols || r >= rows)
		return;
	uint64_t msr = irq_off();
	cells[r][c] = ch;
	attrs[r][c] = (bg & 15) << 4 | (fg & 15);
	if (c == cursor_x && r == cursor_y) {
		draw(c, r);
		cursor_x = cursor_y = -1;
	} else if (cells[r][c] != shown[r][c] || attrs[r][c] != shown_attrs[r][c]) {
		draw(c, r);
	}
	irq_restore(msr);
}

void terminal_goto(int c, int r)
{
	uint64_t msr = irq_off();
	raw_x = c < 0 ? 0 : c >= cols ? cols - 1 : c;
	raw_y = r < 0 ? 0 : r >= rows ? rows - 1 : r;
	show_cursor();
	irq_restore(msr);
}

int terminal_column(void)
{
	return col;
}

static void redraw_line(int n)
{
	int r = n - view_top;
	if (raw || r < 0 || r >= rows)
		return;
	memcpy(cells[r], doc_line(n), cols);
	memcpy(attrs[r], doc_line_attr(n), cols);
	for (int c = 0; c < cols; c++)
		if (cells[r][c] != shown[r][c] || attrs[r][c] != shown_attrs[r][c])
			draw(c, r);
}

static void clamp(void)
{
	if (col < doc_prot[slot(line)])
		col = doc_prot[slot(line)];
	if (col >= cols)
		col = cols - 1;
}

void terminal_mark_prompt(void)
{
	uint64_t msr = irq_off();
	doc_prot[slot(line)] = col < cols ? col : cols;
	irq_restore(msr);
}

void terminal_to_end(void)
{
	uint64_t msr = irq_off();
	hide_cursor();
	line = doc_count - 1;
	col = doc_len[slot(line)];
	follow = 1;
	if (col >= cols)
		newline();
	track();
	show_cursor();
	irq_restore(msr);
}

static void insert_char(char c)
{
	int s = slot(line), n = doc_len[s];
	char *t = doc_line(line);
	uint8_t *a = doc_line_attr(line);
	for (; n < col; n++) {
		t[n] = ' ';
		a[n] = attr;
	}
	if (n >= cols)
		n = cols - 1;		// a full line loses its last character, same as lds
	memmove(t + col + 1, t + col, n - col);
	memmove(a + col + 1, a + col, n - col);
	t[col] = c;
	a[col] = attr;
	doc_len[s] = n + 1;
	redraw_line(line);
	if (++col >= cols) {
		if (line == doc_count - 1) {
			newline();
			doc_cont[slot(line)] = 1;
		} else {
			col = cols - 1;
		}
	}
}

static void delete_char(int at)
{
	int s = slot(line), n = doc_len[s];
	if (at < doc_prot[s] || at >= n)
		return;
	char *t = doc_line(line);
	uint8_t *a = doc_line_attr(line);
	memmove(t + at, t + at + 1, n - at - 1);
	memmove(a + at, a + at + 1, n - at - 1);
	t[n - 1] = ' ';
	doc_len[s] = n - 1;
	redraw_line(line);
}

static int first_of_command(void)
{
	int n = line;
	while (n > 0 && doc_cont[slot(n)])
		n--;
	return n;
}

// esc. a command that wrapped over several lines goes completely
static void clear_command(void)
{
	int first = first_of_command(), l = first + 1;
	for (; l < doc_count && doc_cont[slot(l)]; l++)
		blank_line(l);
	if (l == doc_count)
		doc_count = first + 1;
	line = first;
	int s = slot(line);
	memset(doc_line(line) + doc_prot[s], ' ', COLS_MAX - doc_prot[s]);
	doc_len[s] = doc_prot[s];
	col = doc_prot[s];
	render();
}

void terminal_type(int key)
{
	if (!cols || raw)
		return;
	uint64_t msr = irq_off();
	hide_cursor();
	int s = slot(line);
	int plain = key & ~KEY_MODS;
	if (key >= ' ' && key < 127) {
		clamp();
		insert_char(key);
	} else if (key == 8 || key == 127) {
		if (col > doc_prot[s]) {
			col--;
			delete_char(col);
		} else if (col == 0 && !doc_prot[s] && doc_cont[s] && line > 0) {
			line--;
			col = doc_len[slot(line)] - 1;
			delete_char(col);
		}
	} else if (plain == KEY_DELETE) {
		delete_char(col);
	} else if (plain == KEY_LEFT) {
		if (col > doc_prot[s])
			col--;
		else if (col == 0 && line > 0) {
			line--;
			col = doc_len[slot(line)] < cols ? doc_len[slot(line)] : cols - 1;
		}
		clamp();
	} else if (plain == KEY_RIGHT) {
		if (col < cols - 1)
			col++;
		else if (line < doc_count - 1) {
			line++;
			col = 0;
		}
		clamp();
	} else if (plain == KEY_UP || plain == KEY_DOWN || plain == KEY_PAGE_UP || plain == KEY_PAGE_DOWN) {
		int step = plain == KEY_UP ? -1 : plain == KEY_DOWN ? 1 : plain == KEY_PAGE_UP ? -rows : rows;
		line += step;
		line = line < 0 ? 0 : line >= doc_count ? doc_count - 1 : line;
		clamp();
	} else if (plain == KEY_HOME) {
		col = doc_prot[s];
	} else if (plain == KEY_END) {
		col = doc_len[s] > doc_prot[s] ? doc_len[s] : doc_prot[s];
		clamp();
	} else if (key == KEY_SHIFT_ENTER) {
		if (line == doc_count - 1)
			newline();
		else
			line++;
		col = 0;
	} else if (key == KEY_ESC) {
		clear_command();
	}
	follow = 1;
	track();
	show_cursor();
	irq_restore(msr);
}

int terminal_command(char *buf, int room, int *pos)
{
	uint64_t msr = irq_off();
	int n = 0, at = 0;
	for (int l = first_of_command(); l < doc_count; l++) {
		int s = slot(l);
		if (l > line && !doc_cont[s])
			break;
		for (int c = doc_prot[s]; c < doc_len[s] && n < room - 1; c++) {
			if (l == line && c == col)
				at = n;
			buf[n++] = doc_line(l)[c];
		}
		if (l == line && col >= doc_len[s])
			at = n;
	}
	buf[n] = 0;
	irq_restore(msr);
	if (pos)
		*pos = at;
	return n;
}

void terminal_replace_command(const char *text)
{
	uint64_t msr = irq_off();
	hide_cursor();
	clear_command();
	for (; *text; text++)
		insert_char(*text);
	track();
	show_cursor();
	irq_restore(msr);
}
