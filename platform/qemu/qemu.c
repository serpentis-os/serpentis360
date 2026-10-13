#include "kernel.h"
#include "fdt.h"
#include "ppc.h"
#include "screen.h"

// qemu powernv. not a 360 but 64 bit big endian hv mode, enough for the core

#define OPAL_CONSOLE_WRITE 1
#define OPAL_CONSOLE_READ 2
#define OPAL_CEC_POWER_DOWN 5
#define OPAL_CEC_REBOOT 6
#define SKIBOOT_BASE 0x30000000ull

#define VFB_BASE 0x1f000000ull
#define VFB_W 1280
#define VFB_H 720

// no display on powernv, the screen goes into ram here and i read it out with pmemsave
// from the qemu monitor
static void vfb_pixel(int x, int y, uint32_t rgb)
{
	if (x >= 0 && y >= 0 && x < VFB_W && y < VFB_H)
		((volatile uint32_t *)VFB_BASE)[y * VFB_W + x] = rgb;
}

uint64_t opal_base, opal_entry;
long opal_call(long token, long a, long b, long c);

static uint64_t tb_freq = 512000000;
static uint64_t mem_end = SKIBOOT_BASE;

void platform_init(uint64_t fdt, uint64_t r8, uint64_t r9)
{
	opal_base = r8;
	opal_entry = r9;
	const void *v;
	int len;
	if (!fdt_find(fdt, "cpu", "timebase-frequency", &v, &len))
		tb_freq = fdt_number(v, len);
	screen_init(VFB_W, VFB_H, vfb_pixel);
	if (!fdt_find(fdt, "memory", "reg", &v, &len) && len >= 16) {
		const uint32_t *r = v;
		uint64_t size = (uint64_t)r[2] << 32 | r[3];
		if (size < mem_end)
			mem_end = size;
	}
}

void platform_putc(char c)
{
	uint64_t msr = irq_off();
	for (int tries = 0; tries < 1000; tries++) {
		uint64_t len = 1;
		if (opal_call(OPAL_CONSOLE_WRITE, 0, (long)&len, (long)&c) == 0 && len == 1)
			break;
	}
	irq_restore(msr);
}

int platform_getc(void)
{
	uint64_t msr = irq_off();
	uint64_t len = 1;
	char c;
	long r = opal_call(OPAL_CONSOLE_READ, 0, (long)&len, (long)&c);
	irq_restore(msr);
	return r == 0 && len == 1 ? (unsigned char)c : -1;
}

// the pci on powernv needs the phb set up by hand first, not done yet
int platform_usb_ohci(uint64_t *bases, int max)
{
	return 0;
}

void platform_flush(void)
{
}

uint32_t platform_pvr(void)
{
	return mfspr(287);
}

uint64_t platform_tb_freq(void)
{
	return tb_freq;
}

void platform_memory(uint64_t *start, uint64_t *end)
{
	*start = AS_POOL_START;
	*end = mem_end;
}

const char *platform_name(void)
{
	return "qemu powernv";
}

void platform_reboot(void)
{
	irq_off();
	opal_call(OPAL_CEC_REBOOT, 0, 0, 0);
	for (;;)
		;
}

void platform_poweroff(void)
{
	platform_halt();
}

void platform_halt(void)
{
	irq_off();
	opal_call(OPAL_CEC_POWER_DOWN, 0, 0, 0);
	for (;;)
		;
}
