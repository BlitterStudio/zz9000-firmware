/* Behavioural tests for playback controller (zzplay-controller.h).
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include <stdio.h>
#include <string.h>

#include "../tools/zzplay/zzplay-controls.h"
#include "../tools/zzplay/zzplay-controller.h"

static int test_init_and_text(void)
{
  ZZPlayController ctl;

  zzplay_controller_init(&ctl, 80U, ZZPLAY_REPEAT_ALL, 1);
  if (ctl.volume != 80U) return 2;
  if (ctl.repeat != ZZPLAY_REPEAT_ALL) return 3;
  if (!ctl.shuffle) return 4;
  if (ctl.dirty != ZZPLAY_DIRTY_ALL) return 5;
  ctl.dirty = 0U;
  zzplay_controller_item_begin(&ctl);
  if (!(ctl.dirty & ZZPLAY_DIRTY_CURRENT)) return 6;
  if (ctl.dirty & ZZPLAY_DIRTY_PLAYLIST) return 7;
  if (ctl.jump_index != -1) return 8;
  /* Text setters truncate safely and set DIRTY_INFO. */
  ctl.dirty = 0U;
  zzplay_controller_set_title(&ctl, "My Song Title");
  if (strcmp(ctl.now.title, "My Song Title") != 0) return 7;
  if (!(ctl.dirty & ZZPLAY_DIRTY_INFO)) return 8;

  ctl.dirty = 0U;
  zzplay_controller_set_message(&ctl, "Error reading frame");
  if (strcmp(ctl.now.message, "Error reading frame") != 0) return 9;
  if (!(ctl.dirty & ZZPLAY_DIRTY_INFO)) return 10;

  return 0;
}

static int test_request_precedence(void)
{
  ZZPlayController ctl;
  int32_t jump_idx = -1;
  uint32_t seek_ms = 0U;

  zzplay_controller_init(&ctl, 100U, ZZPLAY_REPEAT_OFF, 0);

  /* Initially no request */
  if (zzplay_controller_item_should_end(&ctl)) return 1;
  if (zzplay_controller_take_request(&ctl, &jump_idx, &seek_ms) != ZZPLAY_REQUEST_NONE)
    return 2;

  /* Latest wins among non-QUIT / non-STOP */
  if (!zzplay_controller_request(&ctl, ZZPLAY_REQUEST_PLAY)) return 3;
  if (!zzplay_controller_request(&ctl, ZZPLAY_REQUEST_NEXT)) return 4;
  if (!zzplay_controller_jump(&ctl, 5)) return 5;
  if (ctl.request != ZZPLAY_REQUEST_JUMP || ctl.jump_index != 5) return 6;

  /* STOP outranks JUMP */
  if (!zzplay_controller_request(&ctl, ZZPLAY_REQUEST_STOP)) return 7;
  if (ctl.request != ZZPLAY_REQUEST_STOP) return 8;

  /* A new NEXT or JUMP cannot overwrite STOP */
  if (zzplay_controller_request(&ctl, ZZPLAY_REQUEST_NEXT) != 0) return 9;
  if (zzplay_controller_jump(&ctl, 2) != 0) return 10;
  if (ctl.request != ZZPLAY_REQUEST_STOP) return 11;

  /* QUIT outranks STOP */
  if (!zzplay_controller_request(&ctl, ZZPLAY_REQUEST_QUIT)) return 12;
  if (ctl.request != ZZPLAY_REQUEST_QUIT) return 13;

  /* Nothing can overwrite QUIT */
  if (zzplay_controller_request(&ctl, ZZPLAY_REQUEST_STOP) != 0) return 14;
  if (zzplay_controller_request(&ctl, ZZPLAY_REQUEST_PLAY) != 0) return 15;
  if (ctl.request != ZZPLAY_REQUEST_QUIT) return 16;

  /* take_request clears request */
  if (zzplay_controller_take_request(&ctl, &jump_idx, &seek_ms) != ZZPLAY_REQUEST_QUIT)
    return 17;
  if (ctl.request != ZZPLAY_REQUEST_NONE) return 18;
  if (zzplay_controller_item_should_end(&ctl)) return 19;

  return 0;
}

static int test_seek_clamping_and_advance(void)
{
  ZZPlayController ctl;

  zzplay_controller_init(&ctl, 100U, ZZPLAY_REPEAT_OFF, 0);
  zzplay_controller_item_begin(&ctl);
  zzplay_controller_set_capabilities(&ctl, 1, 1, 0); /* seekable */
  zzplay_controller_set_duration(&ctl, 100000U);      /* 100s */
  zzplay_controller_set_position(&ctl, 20000U, 1);    /* 20s */

  /* Seek backward clamps at 0 */
  zzplay_controller_set_position(&ctl, 5000U, 1);     /* 5s */
  if (!zzplay_controller_apply(&ctl, ZZPLAY_CONTROL_SEEK_BACK)) return 1;
  if (ctl.request != ZZPLAY_REQUEST_SEEK || ctl.seek_ms != 0U) return 2;
  ctl.request = ZZPLAY_REQUEST_NONE;

  /* Normal seek forward by 10s */
  zzplay_controller_set_position(&ctl, 20000U, 1);    /* 20s */
  if (!zzplay_controller_apply(&ctl, ZZPLAY_CONTROL_SEEK_FORWARD)) return 3;
  if (ctl.request != ZZPLAY_REQUEST_SEEK || ctl.seek_ms != 30000U) return 4;
  ctl.request = ZZPLAY_REQUEST_NONE;

  /* Forward past known end becomes NEXT */
  zzplay_controller_set_position(&ctl, 95000U, 1);    /* 95s + 10s > 100s */
  if (!zzplay_controller_apply(&ctl, ZZPLAY_CONTROL_SEEK_FORWARD)) return 5;
  if (ctl.request != ZZPLAY_REQUEST_NEXT) return 6;
  ctl.request = ZZPLAY_REQUEST_NONE;

  /* A direct seek_to at or past the known end advances: there is no audio
   * left to restart on (dragging the slider to its right edge). */
  if (!zzplay_controller_seek_to(&ctl, 150000U)) return 7;
  if (ctl.request != ZZPLAY_REQUEST_NEXT) return 8;
  ctl.request = ZZPLAY_REQUEST_NONE;
  if (!zzplay_controller_seek_to(&ctl, 99000U)) return 10;
  if (ctl.request != ZZPLAY_REQUEST_SEEK || ctl.seek_ms != 99000U) return 11;
  {
    uint32_t taken = 0U;

    if (zzplay_controller_take_request(&ctl, 0, &taken) !=
            ZZPLAY_REQUEST_SEEK ||
        taken != 99000U) return 12;
    /* A later PLAY restarts from the beginning, not the old seek. */
    if (!zzplay_controller_request(&ctl, ZZPLAY_REQUEST_PLAY)) return 13;
    taken = 1U;
    if (zzplay_controller_take_request(&ctl, 0, &taken) !=
            ZZPLAY_REQUEST_PLAY ||
        taken != 0U) return 14;
  }

  /* Seek fails when not seekable or idle */
  zzplay_controller_set_capabilities(&ctl, 0, 1, 0);
  if (zzplay_controller_seek_to(&ctl, 10000U) != 0) return 9;

  return 0;
}

static int test_volume_and_controls(void)
{
  ZZPlayController ctl;
  uint32_t vol = 0U;

  zzplay_controller_init(&ctl, 95U, ZZPLAY_REPEAT_OFF, 0);

  /* Step up clamps at 100 */
  if (!zzplay_controller_apply(&ctl, ZZPLAY_CONTROL_VOLUME_UP)) return 1;
  if (ctl.volume != 100U) return 2;
  if (!zzplay_controller_take_volume(&ctl, &vol) || vol != 100U) return 3;
  /* Already at 100 -> returns 0 (no change) */
  if (zzplay_controller_apply(&ctl, ZZPLAY_CONTROL_VOLUME_UP) != 0) return 4;

  /* Step down */
  if (!zzplay_controller_apply(&ctl, ZZPLAY_CONTROL_VOLUME_DOWN)) return 5;
  if (ctl.volume != 95U) return 6;

  /* Direct set clamps */
  zzplay_controller_set_volume(&ctl, 120U);
  if (ctl.volume != 100U) return 7;

  /* Repeat cycle */
  ctl.repeat = ZZPLAY_REPEAT_OFF;
  if (!zzplay_controller_apply(&ctl, ZZPLAY_CONTROL_CYCLE_REPEAT)) return 8;
  if (ctl.repeat != ZZPLAY_REPEAT_ONE) return 9;
  if (!zzplay_controller_apply(&ctl, ZZPLAY_CONTROL_CYCLE_REPEAT)) return 10;
  if (ctl.repeat != ZZPLAY_REPEAT_ALL) return 11;
  if (!zzplay_controller_apply(&ctl, ZZPLAY_CONTROL_CYCLE_REPEAT)) return 12;
  if (ctl.repeat != ZZPLAY_REPEAT_OFF) return 13;

  /* Shuffle toggle */
  ctl.shuffle = 0;
  if (!zzplay_controller_apply(&ctl, ZZPLAY_CONTROL_TOGGLE_SHUFFLE)) return 14;
  if (!ctl.shuffle) return 15;
  if (!zzplay_controller_apply(&ctl, ZZPLAY_CONTROL_TOGGLE_SHUFFLE)) return 16;
  if (ctl.shuffle) return 17;

  return 0;
}

static int test_position_dirty_pacing(void)
{
  ZZPlayController ctl;

  zzplay_controller_init(&ctl, 100U, ZZPLAY_REPEAT_OFF, 0);
  zzplay_controller_item_begin(&ctl);

  ctl.dirty = 0U;
  /* First position at 1200ms (second 1, exact 0) */
  zzplay_controller_set_position(&ctl, 1200U, 0);
  if (!(ctl.dirty & ZZPLAY_DIRTY_POSITION)) return 1;

  /* Advance to 1500ms (still second 1, exact 0) -> should NOT dirty position! */
  ctl.dirty = 0U;
  zzplay_controller_set_position(&ctl, 1500U, 0);
  if (ctl.dirty & ZZPLAY_DIRTY_POSITION) return 2;

  /* Advance to 2050ms (second 2) -> dirty position */
  ctl.dirty = 0U;
  zzplay_controller_set_position(&ctl, 2050U, 0);
  if (!(ctl.dirty & ZZPLAY_DIRTY_POSITION)) return 3;

  /* Exactness change at same second -> dirty position */
  ctl.dirty = 0U;
  zzplay_controller_set_position(&ctl, 2100U, 1);
  if (!(ctl.dirty & ZZPLAY_DIRTY_POSITION)) return 4;

  return 0;
}

static int test_modal_gate(void)
{
  ZZPlayController ctl;

  zzplay_controller_init(&ctl, 100U, ZZPLAY_REPEAT_OFF, 0);

  /* Case 1: Idle player -> ready immediately */
  if (!zzplay_controller_begin_modal(&ctl, ZZPLAY_MODAL_ABOUT)) return 1;
  /* Trying to queue a second modal fails */
  if (zzplay_controller_begin_modal(&ctl, ZZPLAY_MODAL_OPEN_FILES) != 0) return 2;
  if (zzplay_controller_modal_ready(&ctl) != ZZPLAY_MODAL_ABOUT) return 3;
  zzplay_controller_end_modal(&ctl);
  if (ctl.modal != ZZPLAY_MODAL_NONE) return 4;

  /* Case 2: Active, unpaused item -> pauses item, ready only after engine confirms */
  zzplay_controller_item_begin(&ctl);
  if (ctl.paused) return 5;

  if (!zzplay_controller_begin_modal(&ctl, ZZPLAY_MODAL_ADD_FILES)) return 6;
  if (!ctl.paused || !ctl.modal_paused) return 7;
  /* Not ready before engine confirms */
  if (zzplay_controller_modal_ready(&ctl) != ZZPLAY_MODAL_NONE) return 8;
  /* A pause key in the same poll must not undo the gate's pause, or the
   * engine never confirms and the requester never opens. */
  (void)zzplay_controller_apply(&ctl, ZZPLAY_CONTROL_TOGGLE_PAUSE);
  if (!ctl.paused) return 20;

  /* Engine confirms pause */
  zzplay_controller_set_engine_paused(&ctl, 1);
  if (zzplay_controller_modal_ready(&ctl) != ZZPLAY_MODAL_ADD_FILES) return 9;

  /* Modal ends -> gate resumes item */
  zzplay_controller_end_modal(&ctl);
  if (ctl.paused || ctl.modal_paused) return 10;
  if (ctl.modal != ZZPLAY_MODAL_NONE) return 11;

  /* Case 3: Item was ALREADY paused by user -> gate does NOT resume on end */
  ctl.paused = 1;
  ctl.engine_paused = 1;
  if (!zzplay_controller_begin_modal(&ctl, ZZPLAY_MODAL_SAVE_PLAYLIST)) return 12;
  if (ctl.modal_paused != 0) return 13; /* Gate did not pause it */
  if (zzplay_controller_modal_ready(&ctl) != ZZPLAY_MODAL_SAVE_PLAYLIST) return 14;

  zzplay_controller_end_modal(&ctl);
  if (!ctl.paused) return 15; /* Still paused! */

  /* Case 4: item_end releases modal pause */
  ctl.paused = 0;
  ctl.engine_paused = 0;
  if (!zzplay_controller_begin_modal(&ctl, ZZPLAY_MODAL_ABOUT)) return 16;
  if (!ctl.modal_paused) return 17;

  /* Item ends while modal was waiting or running */
  zzplay_controller_item_end(&ctl);
  if (ctl.modal_paused != 0) return 18;
  zzplay_controller_end_modal(&ctl);
  if (ctl.paused != 0) return 19;

  return 0;
}

static int test_item_lifecycle(void)
{
  ZZPlayController ctl;

  zzplay_controller_init(&ctl, 100U, ZZPLAY_REPEAT_ONE, 0);
  zzplay_controller_set_message(&ctl, "Persistent message");

  zzplay_controller_item_begin(&ctl);
  if (!ctl.item_active) return 1;
  /* Message line is retained across item boundaries */
  if (strcmp(ctl.now.message, "Persistent message") != 0) return 2;
  if (zzplay_controller_loop_item(&ctl) != 1) return 3;

  /* Fullscreen toggle */
  zzplay_controller_set_capabilities(&ctl, 1, 1, 1); /* has video */
  if (!zzplay_controller_apply(&ctl, ZZPLAY_CONTROL_TOGGLE_FULLSCREEN)) return 4;
  if (!zzplay_controller_take_fullscreen_toggle(&ctl)) return 5;
  if (zzplay_controller_take_fullscreen_toggle(&ctl) != 0) return 6;

  /* Item end resets item_active */
  zzplay_controller_item_end(&ctl);
  if (ctl.item_active != 0) return 7;

  return 0;
}

int main(void)
{
  int rc;

  rc = test_init_and_text();
  if (rc) {
    fprintf(stderr, "test_init_and_text failed: %d\n", rc);
    return 1;
  }
  rc = test_request_precedence();
  if (rc) {
    fprintf(stderr, "test_request_precedence failed: %d\n", rc);
    return 2;
  }
  rc = test_seek_clamping_and_advance();
  if (rc) {
    fprintf(stderr, "test_seek_clamping_and_advance failed: %d\n", rc);
    return 3;
  }
  rc = test_volume_and_controls();
  if (rc) {
    fprintf(stderr, "test_volume_and_controls failed: %d\n", rc);
    return 4;
  }
  rc = test_position_dirty_pacing();
  if (rc) {
    fprintf(stderr, "test_position_dirty_pacing failed: %d\n", rc);
    return 5;
  }
  rc = test_modal_gate();
  if (rc) {
    fprintf(stderr, "test_modal_gate failed: %d\n", rc);
    return 6;
  }
  rc = test_item_lifecycle();
  if (rc) {
    fprintf(stderr, "test_item_lifecycle failed: %d\n", rc);
    return 7;
  }

  printf("zzplay_controller_test: all tests passed\n");
  return 0;
}
