| Entry stub for ZZPlay service modules.
| Copyright (C) 2026, Dimitris Panokostas / BlitterStudio
| SPDX-License-Identifier: GPL-3.0-or-later
|
| Must be the first code in the first hunk: LoadSeg loads the file and
| ZZPlay calls the first instruction after the segment link word.
| Returns const ZZPlayProvider * in D0.

	.text
	.globl	_start
_start:
	jsr	_zzplay_provider_entry
	rts
