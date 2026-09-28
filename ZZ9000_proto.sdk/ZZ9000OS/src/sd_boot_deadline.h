/*
 * Bounded early-boot SD access (plan fast-ram-cfg U3)
 * Copyright (C) 2026, Dimitris Panokostas <midwan@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The vendor SD stack (xsdps.c / diskio.c) contains unbounded polling
 * loops (card-detect settling, command-line reset, ACMD41 ready, RCA
 * acquisition, clock stabilization, voltage switch). The boot-time
 * ZZ9000.CFG load races the Amiga's autoconfig pass, so it arms a
 * deadline before touching the card: every wait on that path then
 * polls sd_boot_deadline_hit() and fails fast instead of hanging.
 *
 * Arming is boot-only. Disarmed (deadline 0), every loop behaves
 * exactly as the vendor driver, so later SD users -- HDF storage,
 * firmware update, config saves -- keep their existing semantics.
 */

#ifndef SD_BOOT_DEADLINE_H
#define SD_BOOT_DEADLINE_H

#include <stdint.h>

/* Absolute XTime deadline in counts; 0 = disarmed. Written once
 * before the bounded load begins and only polled afterwards, so the
 * single volatile u64 needs no lock. */
extern volatile uint64_t sd_boot_deadline_xtime;

/* Set when a bounded wait actually exited because the deadline had
 * passed (the loader maps a fired deadline to its TIMEOUT outcome). */
extern volatile uint8_t sd_boot_deadline_fired;

/* Arm for `ms` milliseconds from now (also clears `fired`). */
void sd_boot_deadline_arm(uint32_t ms);

/* Disarm; later SD waits run unbounded, as before. */
void sd_boot_deadline_disarm(void);

#endif
