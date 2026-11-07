/*
 * Guest-client host harness: mock AmigaOS interface.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef GUEST_MOCK_H
#define GUEST_MOCK_H

#include <stddef.h>

/* Point the mock RTG bitmap arena at the mock card (before guests run). */
void guest_mock_set_card(void *card_base, size_t card_size);
/* Reset library open counts and the VRAM arena cursor (per card setup). */
void guest_mock_init_libraries(void);

/* Every Write() to a guest result file is captured here for the verdict. */
const char *guest_mock_result_log(void);
void guest_mock_result_log_clear(void);

#endif /* GUEST_MOCK_H */
