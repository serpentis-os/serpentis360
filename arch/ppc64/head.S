#include "frame.h"
#include "asm.h"

// qemu powernv: skiboot jumps here in hv real mode, r3 = device tree, r8/r9 = opal base
// and entry. xell on the 360 lands here too, the other threads come in at +0x60

	.section .text.head, "ax"
	.globl	_start
_start:
	b	1f

	. = _start + 0x60
secondary:
	or	%r1, %r1, %r1	// low priority, they never get any work
	b	secondary

1:	mr	%r30, %r3
	mr	%r29, %r8
	mr	%r28, %r9

	// bss first, no C before it is zero
	LOADADDR %r10, __bss_start
	LOADADDR %r11, __bss_end
	li	%r0, 0
2:	cmpld	%r10, %r11
	bge	3f
	std	%r0, 0(%r10)
	addi	%r10, %r10, 8
	b	2b

3:	LOADADDR %r1, boot_stack_top
	li	%r0, 0
	stdu	%r0, -ABI_FRAME(%r1)

	// sprg1 points at the cpu block for good, the trap entry needs it before it has a stack
	LOADADDR %r13, cpu0
	mtsprg1	%r13

	mr	%r3, %r30
	mr	%r4, %r29
	mr	%r5, %r28
	CALL	kmain
4:	b	4b

	.section .bss
	.balign	16
	.globl	boot_stack_top
boot_stack:
	.space	16384
boot_stack_top:
