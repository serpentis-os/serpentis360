// the tar the makefile builds, 512 aligned so the headers line up
	.section .rodata
	.balign 512
	.globl source_tar, source_tar_end
source_tar:
	.incbin SOURCE_TAR
source_tar_end:
	.byte 0
