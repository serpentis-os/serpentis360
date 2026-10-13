#ifndef FRAME_H
#define FRAME_H

/* trap frame, shared between the asm entry and C. offsets in bytes */
#define F_GPR(n)	((n) * 8)
#define F_CR		256
#define F_LR		264
#define F_CTR		272
#define F_XER		280
#define F_SRR0		288
#define F_SRR1		296
#define F_VEC		304
#define FRAME_SIZE	320

#define CPU_R11		0
#define CPU_R12		8
#define CPU_CTR		16
#define CPU_KSTACK	24
#define CPU_CURRENT	32
#define CPU_ID		40
#define CPU_CR		48

#define REDZONE		288
#define ABI_FRAME	128

#ifndef __ASSEMBLER__
#include <stdint.h>

struct frame {
	uint64_t gpr[32];
	uint64_t cr, lr, ctr, xer;
	uint64_t srr0, srr1;
	uint64_t vec;
	uint64_t pad;
};

struct cpu {
	uint64_t r11, r12, ctr;
	uint64_t kstack;
	struct thread *current;
	uint64_t id;
	uint64_t cr;
};

_Static_assert(sizeof(struct frame) == FRAME_SIZE, "frame size");
_Static_assert(__builtin_offsetof(struct frame, vec) == F_VEC, "frame vec");
_Static_assert(__builtin_offsetof(struct cpu, current) == CPU_CURRENT, "cpu current");
#endif

#endif
