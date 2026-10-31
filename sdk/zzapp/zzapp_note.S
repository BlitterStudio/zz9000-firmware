/*
 * ELF note, name ZZAPP, little-endian descriptor. Flags come from
 * -DZZAPP_NOTE_FLAGS= (bit0 display, bit1 audio, bit2 gl). n_type is 1;
 * the contract does not name it.
 *
 * SPDX-License-Identifier: MIT
 */
	.section .note.zzapp, "a", %note
	.balign 4
	.long	6
	.long	24
	.long	1
	.asciz	"ZZAPP"
	.balign 4
	.long	0x50505A41
	.long	0
	.long	0
	.long	0
#ifndef ZZAPP_NOTE_FLAGS
#define ZZAPP_NOTE_FLAGS 0
#endif
	.long	ZZAPP_NOTE_FLAGS
	.long	0
