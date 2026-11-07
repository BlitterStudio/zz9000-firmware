/*
 * ZZ9000 MiniGL Wire Protocol v9 Packet Parser & Validator
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MW_PARSE_H
#define MW_PARSE_H

#include <stdint.h>
#include <stddef.h>
#include "mw_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mw_command {
    uint32_t opcode;
    const uint8_t *payload;
    uint32_t bytes;
} mw_command;

typedef int (*mw_command_visitor)(void *user, const mw_command *cmd);

/*
 * Structural validation of the whole packet before execution.
 * Returns MW_STATUS_OK (0) if valid, MW_STATUS_INVALID (0xfffffffe) if rejected.
 */
uint32_t mw_validate_packet(const uint8_t *packet, uint32_t bytes);
/* As above, also returns the first malformed command opcode (zero when the
 * packet framing fails before an opcode is available). */
uint32_t mw_validate_packet_report(const uint8_t *packet, uint32_t bytes,
                                   uint32_t *failed_opcode);

/*
 * Visits an already structurally validated packet.  Callers MUST first use
 * mw_validate_packet_report(); this avoids repeating its full payload pass.
 */
uint32_t mw_parse_packet_validated_report(const uint8_t *packet, uint32_t bytes,
                                          mw_command_visitor visitor, void *user,
                                          uint32_t *failed_opcode);

/*
 * Visits a packet emitted by the in-process MiniGL front end.  It checks the
 * packet and command framing but deliberately skips payload validation.
 * This MUST NOT be used for host-controlled transport memory.
 */
uint32_t mw_parse_packet_trusted_report(const uint8_t *packet, uint32_t bytes,
                                        mw_command_visitor visitor, void *user,
                                        uint32_t *failed_opcode);

/*
 * Validates then visits a packet.  As mw_parse_packet(), also returns the
 * opcode whose visitor rejected the packet.  Validation failures report the
 * malformed command opcode.
 */
uint32_t mw_parse_packet_report(const uint8_t *packet, uint32_t bytes,
                                mw_command_visitor visitor, void *user,
                                uint32_t *failed_opcode);

/* Executes a packet without needing failure diagnostics. */
uint32_t mw_parse_packet(const uint8_t *packet, uint32_t bytes,
                         mw_command_visitor visitor, void *user);

#ifdef __cplusplus
}
#endif

#endif /* MW_PARSE_H */
