#include "kernel.h"
#include "ppc.h"
#include "screen.h"
#include "usb.h"

void selftest(void);

// on the 360 nothing reaches the tv until it leaves the cache, see platform_flush
static void display_main(void *arg)
{
	for (;;) {
		platform_flush();
		sleep_ms(20);
	}
}

void kmain(uint64_t fdt, uint64_t r8, uint64_t r9)
{
	platform_init(fdt, r8, r9);
	kprintf("\nserpentis360, %s\n", platform_name());
	kprintf("msr %016lx  pir %ld\n", mfmsr(), (long)mfspr(SPR_PIR));
	mm_init();
	trap_init();
	sched_init();
	timer_init();
	ui_init();
	thread_create(display_main, NULL, "display");
#ifdef SELFTEST
	selftest();
#else
	usb_init();
	thread_create(usb_main, NULL, "usb");
	thread_create(shell_main, NULL, "shell");
#endif
	irq_on();
	for (;;)
		__asm__ volatile("or 1, 1, 1" : : : "memory");
}
