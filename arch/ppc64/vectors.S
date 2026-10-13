#include "frame.h"
#include "asm.h"

// copied to physical 0 by trap_init. every vector is 4 instructions so the 0x20 spaced hv
// ones at 0xe00 fit too. sprg0 and sprg2 hold r13 and r12 until the common code gets them

#define VEC(n, common)			\
	. = vectors_start + n;		\
	mtsprg0	%r13;			\
	mtsprg2	%r12;			\
	li	%r12, n;		\
	b	common

	.section .text
	.balign	256
	.globl	vectors_start, vectors_end
vectors_start:
	VEC(0x100, low)
	VEC(0x200, low)
	VEC(0x300, low)
	VEC(0x380, low)
	VEC(0x400, low)
	VEC(0x480, low)
	VEC(0x500, low)
	VEC(0x600, low)
	VEC(0x700, low)
	VEC(0x800, low)
	VEC(0x900, low)
	VEC(0x980, low_hv)
	VEC(0xa00, low)
	VEC(0xb00, low)
	VEC(0xc00, low)
	VEC(0xd00, low)
	VEC(0xe00, low_hv)
	VEC(0xe20, low_hv)
	VEC(0xe40, low_hv)
	VEC(0xe60, low_hv)
	VEC(0xe80, low_hv)
	VEC(0xea0, low_hv)
	VEC(0xf00, low)
	VEC(0xf20, low)
	VEC(0xf40, low)
	VEC(0xf60, low)

	. = vectors_start + 0x1000
low:
	mfsprg1	%r13
	std	%r11, CPU_R11(%r13)
	mfsprg2	%r11
	std	%r11, CPU_R12(%r13)
	mfctr	%r11
	std	%r11, CPU_CTR(%r13)
	mfcr	%r11
	std	%r11, CPU_CR(%r13)
	LOADADDR %r11, trap_entry
	mtctr	%r11
	bctr

// the hv ones come in with hsrr0/hsrr1 instead of srr0/srr1, so they go through htrap_entry
low_hv:
	mfsprg1	%r13
	std	%r11, CPU_R11(%r13)
	mfsprg2	%r11
	std	%r11, CPU_R12(%r13)
	mfctr	%r11
	std	%r11, CPU_CTR(%r13)
	mfcr	%r11
	std	%r11, CPU_CR(%r13)
	LOADADDR %r11, htrap_entry
	mtctr	%r11
	bctr
vectors_end:

// r13 = cpu, r12 = vector, the real r11 r12 ctr are in the cpu block and r13 in sprg0.
// from user mode take the kernel stack, from the kernel skip the red zone and stay
.macro ENTRY name, srr0, srr1, rfi
	.globl	\name
\name:
	mfspr	%r11, \srr1
	andi.	%r11, %r11, 0x4000
	beq	1f
	ld	%r11, CPU_KSTACK(%r13)
	b	2f
1:	addi	%r11, %r1, -REDZONE
2:	clrrdi	%r11, %r11, 4
	addi	%r11, %r11, -FRAME_SIZE
	std	%r1, F_GPR(1)(%r11)
	mr	%r1, %r11

	std	%r0, F_GPR(0)(%r1)
	std	%r2, F_GPR(2)(%r1)
	std	%r3, F_GPR(3)(%r1)
	std	%r4, F_GPR(4)(%r1)
	std	%r5, F_GPR(5)(%r1)
	std	%r6, F_GPR(6)(%r1)
	std	%r7, F_GPR(7)(%r1)
	std	%r8, F_GPR(8)(%r1)
	std	%r9, F_GPR(9)(%r1)
	std	%r10, F_GPR(10)(%r1)
	std	%r14, F_GPR(14)(%r1)
	std	%r15, F_GPR(15)(%r1)
	std	%r16, F_GPR(16)(%r1)
	std	%r17, F_GPR(17)(%r1)
	std	%r18, F_GPR(18)(%r1)
	std	%r19, F_GPR(19)(%r1)
	std	%r20, F_GPR(20)(%r1)
	std	%r21, F_GPR(21)(%r1)
	std	%r22, F_GPR(22)(%r1)
	std	%r23, F_GPR(23)(%r1)
	std	%r24, F_GPR(24)(%r1)
	std	%r25, F_GPR(25)(%r1)
	std	%r26, F_GPR(26)(%r1)
	std	%r27, F_GPR(27)(%r1)
	std	%r28, F_GPR(28)(%r1)
	std	%r29, F_GPR(29)(%r1)
	std	%r30, F_GPR(30)(%r1)
	std	%r31, F_GPR(31)(%r1)
	ld	%r0, CPU_R11(%r13)
	std	%r0, F_GPR(11)(%r1)
	ld	%r0, CPU_R12(%r13)
	std	%r0, F_GPR(12)(%r1)
	mfsprg0	%r0
	std	%r0, F_GPR(13)(%r1)
	ld	%r0, CPU_CTR(%r13)
	std	%r0, F_CTR(%r1)
	mflr	%r0
	std	%r0, F_LR(%r1)
	ld	%r0, CPU_CR(%r13)
	std	%r0, F_CR(%r1)
	mfxer	%r0
	std	%r0, F_XER(%r1)
	mfspr	%r0, \srr0
	std	%r0, F_SRR0(%r1)
	mfspr	%r0, \srr1
	std	%r0, F_SRR1(%r1)
	std	%r12, F_VEC(%r1)

	mr	%r3, %r1
	li	%r0, 0
	stdu	%r0, -ABI_FRAME(%r1)
	CALL	trap
	mr	%r1, %r3

	ld	%r0, F_SRR0(%r1)
	mtspr	\srr0, %r0
	ld	%r0, F_SRR1(%r1)
	mtspr	\srr1, %r0
	ld	%r0, F_LR(%r1)
	mtlr	%r0
	ld	%r0, F_CTR(%r1)
	mtctr	%r0
	ld	%r0, F_XER(%r1)
	mtxer	%r0
	ld	%r0, F_CR(%r1)
	mtcr	%r0
	ld	%r0, F_GPR(0)(%r1)
	ld	%r2, F_GPR(2)(%r1)
	ld	%r3, F_GPR(3)(%r1)
	ld	%r4, F_GPR(4)(%r1)
	ld	%r5, F_GPR(5)(%r1)
	ld	%r6, F_GPR(6)(%r1)
	ld	%r7, F_GPR(7)(%r1)
	ld	%r8, F_GPR(8)(%r1)
	ld	%r9, F_GPR(9)(%r1)
	ld	%r10, F_GPR(10)(%r1)
	ld	%r11, F_GPR(11)(%r1)
	ld	%r12, F_GPR(12)(%r1)
	ld	%r13, F_GPR(13)(%r1)
	ld	%r14, F_GPR(14)(%r1)
	ld	%r15, F_GPR(15)(%r1)
	ld	%r16, F_GPR(16)(%r1)
	ld	%r17, F_GPR(17)(%r1)
	ld	%r18, F_GPR(18)(%r1)
	ld	%r19, F_GPR(19)(%r1)
	ld	%r20, F_GPR(20)(%r1)
	ld	%r21, F_GPR(21)(%r1)
	ld	%r22, F_GPR(22)(%r1)
	ld	%r23, F_GPR(23)(%r1)
	ld	%r24, F_GPR(24)(%r1)
	ld	%r25, F_GPR(25)(%r1)
	ld	%r26, F_GPR(26)(%r1)
	ld	%r27, F_GPR(27)(%r1)
	ld	%r28, F_GPR(28)(%r1)
	ld	%r29, F_GPR(29)(%r1)
	ld	%r30, F_GPR(30)(%r1)
	ld	%r31, F_GPR(31)(%r1)
	ld	%r1, F_GPR(1)(%r1)
	\rfi
.endm

	ENTRY trap_entry, 26, 27, rfid
	ENTRY htrap_entry, 314, 315, hrfid
