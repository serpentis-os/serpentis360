#include "kernel.h"
#include "screen.h"

// no desktop, the whole screen is the terminal. some margin because tvs eat the edges

#define MARGIN 16

void ui_init(void)
{
	if (!screen_ready())
		return;
	screen_fill(0, 0, screen_w, screen_h, BLACK);
	terminal_init(MARGIN, MARGIN, (screen_w - MARGIN * 2) / CELL_W, (screen_h - MARGIN * 2) / CELL_H);
}
