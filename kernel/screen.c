#include "kernel.h"
#include "screen.h"

// the 8x8 font doubled up was unreadable on the tv. grey level glyphs at real size now

extern const uint8_t font_glyphs[95 + SPR_COUNT * 2][CELL_H][CELL_W];

int screen_w, screen_h;
static void (*put)(int x, int y, uint32_t rgb);

static const uint32_t palette[16] = {
	0x000000, 0x2a52be, 0x3ea53e, 0x2fa8b0, 0xc23b3b, 0xa04ab0, 0xc08a2a, 0xbfbfbf,
	0x666666, 0x6a9cff, 0x7be37b, 0x6fe3ec, 0xff7070, 0xf08cf0, 0xf5e66a, 0xffffff
};

void screen_init(int width, int height, void (*put_pixel)(int x, int y, uint32_t rgb))
{
	put = put_pixel;
	screen_w = width;
	screen_h = height;
	screen_fill(0, 0, screen_w, screen_h, BLACK);
}

int screen_ready(void)
{
	return put != NULL;
}

void screen_fill(int x, int y, int w, int h, int color)
{
	uint32_t rgb = palette[color & 15];
	for (int j = y; j < y + h; j++)
		for (int i = x; i < x + w; i++)
			put(i, j, rgb);
}

// per channel, fg by a and bg by 255 - a
static uint32_t mix(uint32_t f, uint32_t b, int a)
{
	uint32_t out = 0;
	for (int s = 0; s < 24; s += 8) {
		int fc = f >> s & 0xff, bc = b >> s & 0xff;
		out |= (uint32_t)((fc * a + bc * (255 - a) + 127) / 255) << s;
	}
	return out;
}

// past 126 are the pacman sprites
void screen_char(int x, int y, char c, int fg, int bg)
{
	unsigned char ch = c;
	int at;
	if (ch >= 32 && ch <= 126)
		at = ch - 32;
	else if (ch >= SPRITE(0) && ch < SPRITE(SPR_COUNT))
		at = 95 + ch - SPRITE(0);
	else
		at = ch ? '?' - 32 : 0;
	const uint8_t (*g)[CELL_W] = font_glyphs[at];
	uint32_t f = palette[fg & 15], b = palette[bg & 15];
	for (int row = 0; row < CELL_H; row++)
		for (int col = 0; col < CELL_W; col++) {
			int a = g[row][col];
			put(x + col, y + row, a == 0 ? b : a == 255 ? f : mix(f, b, a));
		}
}

void screen_text(int x, int y, const char *s, int fg, int bg)
{
	for (; *s; s++, x += CELL_W)
		screen_char(x, y, *s, fg, bg);
}
