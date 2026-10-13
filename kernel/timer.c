#include "kernel.h"
#include "ppc.h"

volatile uint64_t ticks;
static uint64_t reload;

void timer_init(void)
{
	reload = platform_tb_freq() / HZ;
	mtspr(SPR_DEC, reload);
	kprintf("timer: timebase %ld hz, tick every %ld\n", (long)platform_tb_freq(), (long)reload);
}

// the decrementer fires at zero and has to be loaded again every time
void timer_tick(void)
{
	mtspr(SPR_DEC, reload);
	ticks++;
	sched_wake();
}
