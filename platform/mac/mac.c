#include "kernel.h"
#include "ppc.h"
#include "screen.h"

// test box only, qemu mac99 with usb on the apple ohci

#define MACIO 0x80000000ull
#define ESCC_CTRL (MACIO + 0x13020)
#define ESCC_DATA (MACIO + 0x13030)
#define OHCI_BASE 0x80080000ull
#define VFB_BASE 0x1f000000ull
#define VFB_W 1280
#define VFB_H 720

static volatile uint8_t *ctrl = (volatile uint8_t *)ESCC_CTRL;
static volatile uint8_t *data = (volatile uint8_t *)ESCC_DATA;

static void vfb_pixel(int x, int y, uint32_t rgb)
{
	if (x >= 0 && y >= 0 && x < VFB_W && y < VFB_H)
		((volatile uint32_t *)VFB_BASE)[y * VFB_W + x] = rgb;
}

void platform_init(uint64_t fdt, uint64_t r8, uint64_t r9)
{
	// openbios only ever sent on the escc, wr3 = 8 bit chars, receiver on
	*ctrl = 3;
	*ctrl = 0xc1;
	screen_init(VFB_W, VFB_H, vfb_pixel);
}

void platform_putc(char c)
{
	for (int spin = 0; spin < 100000 && !(*ctrl & 4); spin++)
		;
	*data = c;
}

int platform_getc(void)
{
	if (*ctrl & 1)
		return *data;
	return -1;
}

void platform_flush(void)
{
}

uint32_t platform_pvr(void)
{
	return mfspr(287);
}

// measured against the wall clock, qemu runs the g5 timebase at 25 MHz
uint64_t platform_tb_freq(void)
{
	return 25000000;
}

void platform_memory(uint64_t *start, uint64_t *end)
{
	*start = AS_POOL_START;
	*end = 0x10000000;
}

const char *platform_name(void)
{
	return "qemu mac99";
}

int platform_usb_ohci(uint64_t *bases, int max)
{
	if (max < 1)
		return 0;
	bases[0] = OHCI_BASE;
	return 1;
}

void platform_halt(void)
{
	irq_off();
	kprintf("\nsystem halted\n");
	for (;;)
		;
}

void platform_reboot(void)
{
	platform_halt();
}

void platform_poweroff(void)
{
	platform_halt();
}
