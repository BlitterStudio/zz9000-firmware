/*
 * Minimal entry point for the standalone sound DataType contract harness.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

    .text
    .globl _start
_start:
    move.l 4.w,_SysBase
    jsr _main
    rts
