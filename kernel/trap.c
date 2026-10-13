#include "kernel.h"
#include "ppc.h"

#define SPR_DSISR 18
#define SPR_DAR 19

struct cpu cpu0;

extern char vectors_start[], vectors_end[];

// the vectors live at physical 0, which is whatever the firmware left there. linux does the
// same thing on powernv, so skiboot is fine with it
void trap_init(void)
{
	uint64_t len = vectors_end - vectors_start;
	volatile char *low = (volatile char *)0;
	for (uint64_t i = 0; i < len; i++)
		low[i] = vectors_start[i];
	sync_icache((void *)0, len);
	mtspr(SPR_HDEC, 0x7fffffff);
	cpu0.id = 0;
	mtspr(SPR_SPRG1, &cpu0);
	kprintf("vectors: %ld bytes at 0\n", (long)len);
}

static const char *trap_name(uint64_t vec)
{
	switch (vec) {
	case 0x100: return "system reset";
	case 0x200: return "machine check";
	case 0x300: return "data storage";
	case 0x380: return "data segment";
	case 0x400: return "instruction storage";
	case 0x480: return "instruction segment";
	case 0x500: return "external interrupt";
	case 0x600: return "alignment";
	case 0x700: return "program";
	case 0x800: return "fp unavailable";
	case 0xe40: return "illegal instruction";
	case 0xe60: return "hypervisor maintenance";
	case 0xf20: return "vector unavailable";
	}
	return "unknown trap";
}

void dump_frame(const struct frame *f)
{
	for (int i = 0; i < 32; i += 4)
		kprintf("r%-2d %016lx  r%-2d %016lx  r%-2d %016lx  r%-2d %016lx\n", i, f->gpr[i], i + 1,
			f->gpr[i + 1], i + 2, f->gpr[i + 2], i + 3, f->gpr[i + 3]);
	kprintf("pc  %016lx  msr %016lx  lr  %016lx  ctr %016lx\n", f->srr0, f->srr1, f->lr, f->ctr);
	kprintf("cr  %08lx  xer %08lx  trap %lx\n", f->cr, f->xer, f->vec);
}

// gives back the frame to return to, another thread's when the scheduler switched.
// anything that is not the timer, sc or hdec is a crash
struct frame *trap(struct frame *f)
{
	switch (f->vec) {
	case 0x900:
		timer_tick();
		return schedule(f);
	case 0xc00:
		return schedule(f);
	case 0x980:
		mtspr(SPR_HDEC, 0x7fffffff);
		return f;
	}
	kprintf("\n%s at %016lx", trap_name(f->vec), f->srr0);
	if (f->vec == 0x300)
		kprintf(", address %016lx dsisr %08lx", mfspr(SPR_DAR), mfspr(SPR_DSISR));
	kprintf(", thread %s\n", cpu0.current ? cpu0.current->name : "?");
	dump_frame(f);
	panic("%s", trap_name(f->vec));
}
