#ifndef SCREEN_H
#define SCREEN_H

#include <stdint.h>

enum {
	BLACK, BLUE, GREEN, CYAN, RED, MAGENTA, BROWN, LIGHT_GRAY,
	DARK_GRAY, LIGHT_BLUE, LIGHT_GREEN, LIGHT_CYAN, LIGHT_RED, LIGHT_MAGENTA, YELLOW, WHITE
};

#define CELL_W 10
#define CELL_H 20

// pacman sprites after the ascii in the font, each one two cells wide
enum {
	SPR_DOT, SPR_PELLET, SPR_PAC_R, SPR_PAC_L, SPR_PAC_U, SPR_PAC_D, SPR_PAC, SPR_GHOST, SPR_SCARED,
	SPR_WALL, SPR_COUNT
};
#define SPRITE(n) (128 + (n) * 2)

extern int screen_w, screen_h;

void screen_init(int width, int height, void (*put_pixel)(int x, int y, uint32_t rgb));
int screen_ready(void);
void screen_fill(int x, int y, int w, int h, int color);
void screen_char(int x, int y, char c, int fg, int bg);
void screen_text(int x, int y, const char *s, int fg, int bg);

void ui_init(void);

void terminal_init(int x, int y, int cols, int rows);
void terminal_putc(char c);
void terminal_color(int fg);
void terminal_clear(void);
void terminal_cursor(int on);
int terminal_ready(void);
int terminal_cols(void);
int terminal_rows(void);
void terminal_cell(int c, int r, char ch, int fg, int bg);
void terminal_goto(int c, int r);
void terminal_scroll(int delta);
void terminal_raw(int on);
int terminal_column(void);
void terminal_mark_prompt(void);
void terminal_to_end(void);
void terminal_type(int key);
int terminal_command(char *buf, int room, int *pos);
void terminal_replace_command(const char *text);

#endif
