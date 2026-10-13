#include "kernel.h"
#include "ppc.h"
#include "usb.h"
#include "keys.h"

// usb and the serial port end up here as the same key codes
#define QUEUE 256

static int queue[QUEUE];
static int head, count;

void input_push(int c)
{
	uint64_t msr = irq_off();
	if (count < QUEUE)
		queue[(head + count++) % QUEUE] = c;
	irq_restore(msr);
}

static int pop(void)
{
	uint64_t msr = irq_off();
	int c = -1;
	if (count) {
		c = queue[head];
		head = (head + 1) % QUEUE;
		count--;
	}
	irq_restore(msr);
	return c;
}

// a lone ESC is only lone after a short wait
static int serial_wait(void)
{
	for (int i = 0; i < 6; i++) {
		int c = platform_getc();
		if (c >= 0)
			return c;
		sleep_ms(5);
	}
	return -1;
}

static int pending = -1;

static int serial_key(void)
{
	int c = pending >= 0 ? pending : platform_getc();
	pending = -1;
	if (c != 27)
		return c == '\r' ? '\n' : c;
	int b = serial_wait();
	if (b != '[' && b != 'O') {
		pending = b;
		return KEY_ESC;
	}
	int n = 0, mod = 0, k = serial_wait();
	while (k >= '0' && k <= '9') {
		n = n * 10 + k - '0';
		k = serial_wait();
	}
	if (k == ';') {
		k = serial_wait();
		while (k >= '0' && k <= '9') {
			mod = mod * 10 + k - '0';
			k = serial_wait();
		}
	}
	int mods = 0;
	if (mod >= 2) {
		mod--;
		mods = (mod & 1 ? KEY_SHIFT : 0) | (mod & 2 ? KEY_ALT : 0) | (mod & 4 ? KEY_CTRL : 0);
	}
	switch (k) {
	case 'A': return KEY_UP | mods;
	case 'B': return KEY_DOWN | mods;
	case 'C': return KEY_RIGHT | mods;
	case 'D': return KEY_LEFT | mods;
	case 'H': return KEY_HOME | mods;
	case 'F': return KEY_END | mods;
	case 'P': case 'Q': case 'R': case 'S': return (KEY_F1 + k - 'P') | mods;
	case '~':
		switch (n) {
		case 1: case 7: return KEY_HOME | mods;
		case 4: case 8: return KEY_END | mods;
		case 2: return KEY_INSERT | mods;
		case 3: return KEY_DELETE | mods;
		case 5: return KEY_PAGE_UP | mods;
		case 6: return KEY_PAGE_DOWN | mods;
		case 15: return (KEY_F1 + 4) | mods;
		case 17: case 18: case 19: case 20: case 21: return (KEY_F1 + 5 + n - 17) | mods;
		case 23: case 24: return (KEY_F1 + 10 + n - 23) | mods;
		}
	}
	return -1;
}

void input_push_str(const char *s)
{
	while (*s)
		input_push((unsigned char)*s++);
}

// usb first, serial only when nothing is queued
int input_getc(void)
{
	int c = pop();
	if (c < 0)
		c = serial_key();
	return c;
}
