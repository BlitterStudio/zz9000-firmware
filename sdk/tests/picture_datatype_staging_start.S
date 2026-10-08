/*
 * Minimal entry point for the standalone picture DataType staging harness.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

    .text
    .globl _start
_start:
    jsr _main
    rts
