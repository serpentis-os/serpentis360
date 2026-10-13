#ifndef PPC_H
#define PPC_H

#include <stdint.h>

#define SPR_DEC		22
#define SPR_SRR0	26
#define SPR_SRR1	27
#define SPR_SPRG1	273
#define SPR_HDEC	310
#define SPR_LPCR	318
#define SPR_PIR		1023

#define MSR_SF		(1ull << 63)
#define MSR_HV		(1ull << 60)
#define MSR_EE		(1ull << 15)
#define MSR_PR		(1ull << 14)
#define MSR_ME		(1ull << 12)
#define MSR_IR		(1ull << 5)
#define MSR_DR		(1ull << 4)
#define MSR_RI		(1ull << 1)

// the spr number is part of the instruction, so these have to be macros
#define mfspr(n) ({ uint64_t v_; __asm__ volatile("mfspr %0, %1" : "=r"(v_) : "i"(n)); v_; })
#define mtspr(n, v) __asm__ volatile("mtspr %0, %1" : : "i"(n), "r"((uint64_t)(v)) : "memory")

static inline uint64_t mfmsr(void)
{
	uint64_t v;
	__asm__ volatile("mfmsr %0" : "=r"(v));
	return v;
}

static inline void mtmsrd(uint64_t v)
{
	__asm__ volatile("mtmsrd %0" : : "r"(v) : "memory");
}

static inline uint64_t mftb(void)
{
	uint64_t v;
	__asm__ volatile("mftb %0" : "=r"(v));
	return v;
}

static inline void irq_on(void)
{
	mtmsrd(mfmsr() | MSR_EE);
}

static inline uint64_t irq_off(void)
{
	uint64_t msr = mfmsr();
	mtmsrd(msr & ~MSR_EE);
	return msr;
}

static inline void irq_restore(uint64_t msr)
{
	mtmsrd(msr);
}

static inline void sync_icache(void *start, uint64_t len)
{
	for (uint64_t a = (uint64_t)start & ~127ull; a < (uint64_t)start + len; a += 128)
		__asm__ volatile("dcbst 0, %0" : : "r"(a) : "memory");
	__asm__ volatile("sync" : : : "memory");
	for (uint64_t a = (uint64_t)start & ~127ull; a < (uint64_t)start + len; a += 128)
		__asm__ volatile("icbi 0, %0" : : "r"(a) : "memory");
	__asm__ volatile("sync; isync" : : : "memory");
}

#endif
