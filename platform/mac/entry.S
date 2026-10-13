#include "asm.h"

// qemu mac99, a g5 with openbios. qemu puts the image at (link & 0x0fffffff) + 16 MB and
// openbios jumps there in 32 bit mode with its own mmu on. so: go 64 bit real mode, copy
// the image down to where it was linked and enter the normal _start. only used for testing
// usb, the 360 has the same ohci

	.section .text.entry, "ax"
	.globl	mac_entry
mac_entry:
	bl	1f
1:	mflr	%r20
	addi	%r21, %r20, 2f - 1b
	mtsrr0	%r21
	lis	%r22, 0x8000
	sldi	%r22, %r22, 32
	ori	%r22, %r22, 0x1000
	mtsrr1	%r22
	rfid

2:	addi	%r20, %r20, mac_entry - 1b
	LOADADDR %r21, __image_start
	LOADADDR %r22, __image_end
	sub	%r23, %r22, %r21
	srdi	%r23, %r23, 3
	mtctr	%r23
	mr	%r24, %r21
3:	ld	%r25, 0(%r20)
	std	%r25, 0(%r24)
	addi	%r20, %r20, 8
	addi	%r24, %r24, 8
	bdnz	3b

	mr	%r24, %r21
4:	dcbst	0, %r24
	addi	%r24, %r24, 128
	cmpld	%r24, %r22
	blt	4b
	sync
	mr	%r24, %r21
5:	icbi	0, %r24
	addi	%r24, %r24, 128
	cmpld	%r24, %r22
	blt	5b
	sync
	isync

	li	%r3, 0
	li	%r8, 0
	li	%r9, 0
	LOADADDR %r21, _start
	mtctr	%r21
	bctr
