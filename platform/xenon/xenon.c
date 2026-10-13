#include "kernel.h"
#include "fdt.h"
#include "ppc.h"
#include "screen.h"

// xell leaves LPCR[RMI] set, real mode is cache inhibited. 0x8000... skips HRMOR

#define REAL(pa) (0x8000000000000000ull | (pa))
#define SOC(off) ((volatile uint32_t *)REAL(0x20000000000ull | (off)))

#define UART_RX 0xea001010
#define UART_TX 0xea001014
#define UART_STATUS 0xea001018
#define IIC_BASE 0x20000050000ull
#define GPU_FB 0xec806110	// scanout base, then 8 words later width and height
#define LPCR_RMI 2

static uint64_t tb_freq = 50000000;
static uint64_t mem_end = AS_POOL_END;	// above that the gpu owns it
static volatile uint32_t *fb;
static int fb_w, fb_h, fb_pitch;
static uint64_t dirty_lo = ~0ull, dirty_hi;

static void uart_putc(char c)
{
	for (int spin = 0; spin < 100000; spin++)
		if (*SOC(UART_STATUS) & 0x02000000) {
			*SOC(UART_TX) = (uint32_t)(unsigned char)c << 24;
			return;
		}
}

// 32x32 tiles, the address math libxenon's console uses
static void put_pixel(int x, int y, uint32_t rgb)
{
	if (x < 0 || y < 0 || x >= fb_w || y >= fb_h)
		return;
	int index = ((y >> 5) * 32 * fb_pitch + ((x >> 5) << 10) + (x & 3) + ((y & 1) << 2)
		     + (((x & 31) >> 2) << 3) + (((y & 31) >> 1) << 6)) ^ ((y & 8) << 2);
	uint32_t r = rgb >> 16 & 0xff, g = rgb >> 8 & 0xff, b = rgb & 0xff;
	fb[index] = b << 24 | g << 16 | r << 8;
	uint64_t at = (uint64_t)&fb[index];
	if (at < dirty_lo)
		dirty_lo = at;
	if (at > dirty_hi)
		dirty_hi = at;
}

// the gpu reads ram, not the l2. first boot on the real box had half drawn letters
void platform_flush(void)
{
	uint64_t msr = irq_off();
	uint64_t lo = dirty_lo, hi = dirty_hi;
	dirty_lo = ~0ull;
	dirty_hi = 0;
	irq_restore(msr);
	if (lo > hi)
		return;
	for (uint64_t a = lo & ~127ull; a <= hi; a += 128)
		__asm__ volatile("dcbst 0, %0" : : "r"(a) : "memory");
	__asm__ volatile("sync" : : : "memory");
}

void platform_init(uint64_t fdt, uint64_t r8, uint64_t r9)
{
	mtspr(SPR_LPCR, mfspr(SPR_LPCR) | LPCR_RMI);
	__asm__ volatile("isync");

	// 0x7c masks the externals, the decrementer still comes in. libxenon does it too
	uint64_t pir = mfspr(SPR_PIR);
	*(volatile uint64_t *)REAL(IIC_BASE + pir * 0x1000 + 8) = 0x7c;

	const void *v;
	int len;
	if (!fdt_find(fdt, NULL, "timebase-frequency", &v, &len))
		tb_freq = fdt_number(v, len);

	uint32_t base = SOC(GPU_FB)[0];
	fb_w = SOC(GPU_FB)[9];
	fb_h = SOC(GPU_FB)[10];
	if (base && fb_w > 0 && fb_h > 0 && fb_w <= 1920 && fb_h <= 1200) {
		fb = (volatile uint32_t *)(uint64_t)base;
		fb_pitch = ((fb_w + 31) >> 5) << 5;
		if (base < mem_end)
			mem_end = base & ~0xffffull;
		screen_init(fb_w, fb_h, put_pixel);
	}
}

void platform_putc(char c)
{
	uart_putc(c);
}

// nothing on the uart header reads back as endless enters
int platform_getc(void)
{
	return -1;
}

// xell leaves the ehci owning the ports. halt it and drop CONFIGFLAG so they fall to
// ohci. the 0x44 writes are copied from libxenon
int platform_usb_ohci(uint64_t *bases, int max)
{
	static const uint64_t ehci[2] = {0xea003000, 0xea005000};
	static const uint64_t ohci_cfg[2] = {0xd0120044, 0xd0128044};
	static const uint64_t ohci[2] = {0xea002000, 0xea004000};
	int n = 0;
	for (int i = 0; i < 2 && n < max; i++) {
		*SOC(ehci[i] + 0x28) = 0;
		*SOC(ehci[i] + 0x20) = 0;
		for (int spin = 0; spin < 100000 && !(__builtin_bswap32(*SOC(ehci[i] + 0x24)) & 0x1000); spin++)
			;
		*SOC(ehci[i] + 0x60) = 0;
		*SOC(ohci_cfg[i]) = __builtin_bswap32(0xed44);
		bases[n++] = REAL(0x20000000000ull | ohci[i]);
	}
	return n;
}

uint32_t platform_pvr(void)
{
	return mfspr(287);
}

uint64_t platform_tb_freq(void)
{
	return tb_freq;
}

// the pool stops where the framebuffer starts
void platform_memory(uint64_t *start, uint64_t *end)
{
	extern char __kernel_end[];
	*start = ((uint64_t)__kernel_end + 0xffff) & ~0xffffull;
	*end = mem_end;
}

const char *platform_name(void)
{
	return "xbox 360";
}

// smc fifo, 16 byte messages. the control word is little endian, the message is not
static void smc_send(const uint32_t msg[4])
{
	volatile uint32_t *ctrl = SOC(0xea001084), *fifo = SOC(0xea001080);
	for (int spin = 0; spin < 1000000 && !(__builtin_bswap32(*ctrl) & 4); spin++)
		;
	*ctrl = __builtin_bswap32(4);
	for (int i = 0; i < 4; i++)
		*fifo = msg[i];
	*ctrl = 0;
}

void platform_reboot(void)
{
	static const uint32_t msg[4] = {0x82043000};
	irq_off();
	smc_send(msg);
	for (;;)
		;
}

void platform_poweroff(void)
{
	static const uint32_t msg[4] = {0x82010000};
	irq_off();
	smc_send(msg);
	for (;;)
		;
}

void platform_halt(void)
{
	irq_off();
	for (;;)
		__asm__ volatile("or 1, 1, 1");
}
