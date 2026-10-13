#ifndef KEYS_H
#define KEYS_H

// below 0x100 plain ascii, above it a key code | modifiers

#define KEY_UP 0x100
#define KEY_DOWN 0x101
#define KEY_RIGHT 0x102
#define KEY_LEFT 0x103
#define KEY_HOME 0x104
#define KEY_END 0x105
#define KEY_PAGE_UP 0x106
#define KEY_PAGE_DOWN 0x107
#define KEY_INSERT 0x108
#define KEY_DELETE 0x109
#define KEY_F1 0x111		// up to KEY_F1 + 11
#define KEY_SHIFT_ENTER 0x120

#define KEY_SHIFT 0x1000
#define KEY_CTRL 0x2000
#define KEY_ALT 0x4000
#define KEY_MODS (KEY_SHIFT | KEY_CTRL | KEY_ALT)

#define KEY_ESC 27
#define KEY_INTERRUPT 3		// ctrl c

#endif
