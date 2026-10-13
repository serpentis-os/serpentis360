#include "kernel.h"
#include "ppc.h"
#include "usb.h"
#include "keys.h"

// boot protocol only. repeat is done here, the keyboard only sends changes

#define REPEAT_DELAY (HZ / 2)
#define REPEAT_RATE (HZ / 25)

#define MOD_CTRL 0x11
#define MOD_SHIFT 0x22
#define MOD_ALT 0x44

#define LED_NUM 1
#define LED_CAPS 2

int mouse_x = 320, mouse_y = 180, mouse_buttons;

static const char plain[] = "abcdefghijklmnopqrstuvwxyz1234567890\n\033\b\t -=[]\\#;'`,./";
static const char shifted[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\n\033\b\t _+{}|~:\"~<>?";

static int held_usage, held_mods;
static uint64_t repeat_at;
static int locks = LED_NUM;
static struct usb_dev *keyboard;

static void set_leds(struct usb_dev *d)
{
	uint8_t setup[8] = {0x21, 0x09, 0, 0x02, d->iface, 0, 1, 0};
	uint8_t leds = locks;
	ohci_control(d->hc, d->addr, d->low_speed, d->mps0, setup, &leds, 1);
}

// hid usage codes to what input_getc hands out. us layout only
// TODO other layouts, at least a way to pick one
static int code_of(int usage, int mods)
{
	int shift = mods & MOD_SHIFT, ctrl = mods & MOD_CTRL, alt = mods & MOD_ALT;
	int keymods = (shift ? KEY_SHIFT : 0) | (ctrl ? KEY_CTRL : 0) | (alt ? KEY_ALT : 0);
	if (usage >= 0x04 && usage <= 0x38) {
		int upper = shift;
		if (usage <= 0x1d && (locks & LED_CAPS))
			upper = !upper;
		char c = (upper ? shifted : plain)[usage - 0x04];
		if (usage == 0x28)
			return shift ? KEY_SHIFT_ENTER : '\n';
		if (usage == 0x2a)
			return 8;
		if (ctrl && c >= 'a' && c <= 'z')
			return c & 0x1f;
		if (ctrl && c >= 'A' && c <= 'Z')
			return c & 0x1f;
		return alt ? (c | KEY_ALT) : c;
	}
	if (usage >= 0x3a && usage <= 0x45)
		return (KEY_F1 + usage - 0x3a) | keymods;
	switch (usage) {
	case 0x49: return KEY_INSERT | keymods;
	case 0x4a: return KEY_HOME | keymods;
	case 0x4b: return KEY_PAGE_UP | keymods;
	case 0x4c: return KEY_DELETE | keymods;
	case 0x4d: return KEY_END | keymods;
	case 0x4e: return KEY_PAGE_DOWN | keymods;
	case 0x4f: return KEY_RIGHT | keymods;
	case 0x50: return KEY_LEFT | keymods;
	case 0x51: return KEY_DOWN | keymods;
	case 0x52: return KEY_UP | keymods;
	}
	if (usage >= 0x54 && usage <= 0x63) {
		static const char pad[] = "/*-+\n1234567890.";
		static const int nav[] = {0, 0, 0, 0, 0, KEY_END, KEY_DOWN, KEY_PAGE_DOWN, KEY_LEFT, 0,
					  KEY_RIGHT, KEY_HOME, KEY_UP, KEY_PAGE_UP, KEY_INSERT, KEY_DELETE};
		int i = usage - 0x54;
		if ((locks & LED_NUM) || !nav[i] || i < 5)
			return pad[i];
		return nav[i] | keymods;
	}
	return -1;
}

void hid_attach(struct usb_dev *d)
{
	uint8_t setup[8] = {0x21, 0x0b, 0, 0, d->iface, 0, 0, 0};	// set protocol: boot
	ohci_control(d->hc, d->addr, d->low_speed, d->mps0, setup, NULL, 0);
	uint8_t idle[8] = {0x21, 0x0a, 0, 0, d->iface, 0, 0, 0};
	ohci_control(d->hc, d->addr, d->low_speed, d->mps0, idle, NULL, 0);
	int len = d->kind == USB_KEYBOARD ? 8 : (d->ep_in_mps < 8 ? d->ep_in_mps : 8);
	d->pipe = ohci_int_open(d->hc, d->addr, d->ep_in, d->low_speed, d->ep_in_mps, len);
	if (d->kind == USB_KEYBOARD) {
		keyboard = d;
		set_leds(d);
	}
}

static int contains(const uint8_t *keys, int usage)
{
	for (int i = 2; i < 8; i++)
		if (keys[i] == usage)
			return 1;
	return 0;
}

static void press(struct usb_dev *d, int usage, int mods)
{
	if (usage == 0x39 || usage == 0x53) {
		locks ^= usage == 0x39 ? LED_CAPS : LED_NUM;
		set_leds(d);
		return;
	}
	int code = code_of(usage, mods);
	if (code < 0)
		return;
	input_push(code);
	held_usage = usage;
	held_mods = mods;
	repeat_at = ticks + REPEAT_DELAY;
}

// a key counts as pressed when it shows up in a report it was not in before
void hid_report(struct usb_dev *d, const uint8_t *r, int len)
{
	if (d->kind == USB_MOUSE && len >= 3) {
		mouse_buttons = r[0] & 7;
		mouse_x += (int8_t)r[1];
		mouse_y += (int8_t)r[2];
		mouse_x = mouse_x < 0 ? 0 : mouse_x > 639 ? 639 : mouse_x;
		mouse_y = mouse_y < 0 ? 0 : mouse_y > 359 ? 359 : mouse_y;
		return;
	}
	if (d->kind != USB_KEYBOARD || len < 8 || r[2] == 1)	// 1 = rollover, too many keys at once
		return;
	for (int i = 2; i < 8; i++)
		if (r[i] && !contains(d->last, r[i]))
			press(d, r[i], r[0]);
	if (held_usage && !contains(r, held_usage))
		held_usage = 0;
	else if (held_usage)
		held_mods = r[0];
	memcpy(d->last, r, 8);
}

void hid_repeat(void)
{
	// enter never repeats. on the 360 a slow stick write once let a held enter through
	// twice and the next question got an empty answer
	if (!held_usage || ticks < repeat_at || held_usage == 0x28 || held_usage == 0x58)
		return;
	int code = code_of(held_usage, held_mods);
	if (code >= 0)
		input_push(code);
	repeat_at = ticks + REPEAT_RATE;
}

void hid_detach(struct usb_dev *d)
{
	if (keyboard == d) {
		keyboard = NULL;
		held_usage = 0;
	}
}
