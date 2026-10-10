| Entry for the ZZ9000-MP3 descriptor DTCD hook.
| Copyright (C) 2026, Dimitris Panokostas / BlitterStudio
| SPDX-License-Identifier: GPL-3.0-or-later
|
| Must be the first code in the first hunk: datatypes.library calls the hunk
| start with struct DTHookContext * in A0 and reads the result from D0.

	.text
	.globl	_start
_start:
	move.l	a0,-(sp)
	jsr	_zz9k_mp3_dthook
	addq.l	#4,sp
	rts
