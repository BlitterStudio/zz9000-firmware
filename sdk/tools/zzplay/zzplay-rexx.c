/* ZZPlay's ARexx port; see zzplay-rexx.h.
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "zzplay-rexx.h"

#include <exec/ports.h>
#include <exec/types.h>
#include <rexx/errors.h>
#include <rexx/storage.h>

#include <proto/exec.h>
#include <proto/rexxsyslib.h>

#include <stdio.h>
#include <string.h>

#include "zzplay-files.h"
#include "zzplay-gui.h"
#include "zzplay-requester.h"
#include "zzplay-rexx-commands.h"

/* Initialised, so libnix does not link an auto-open stub that would refuse
 * to start ZZPlay on a system without ARexx. */
struct RxsLib *RexxSysBase = 0;

#define ZZPLAY_REXX_BASE_NAME "ZZPLAY"
#define ZZPLAY_REXX_MAX_SUFFIX 99

static struct MsgPort *rexx_port;
static char rexx_port_name[sizeof(ZZPLAY_REXX_BASE_NAME) + 3];
static ZZPlayRexxHost rexx_host;
/* A gated command waiting for the modal gate. Nothing else is taken from
 * the port meanwhile, so `rexx_line` stays its text until it runs. */
static struct RexxMsg *rexx_held;
static char rexx_line[ZZPLAY_REXX_LINE_MAX];
static ZZPlayRexxReply rexx_reply;

static uint32_t rexx_add_path(void *user, ZZPlayPlaylist *list,
                              const char *path)
{
  (void)user;
  return zzplay_files_add(list, path);
}

static int rexx_save_playlist(void *user, const ZZPlayPlaylist *list,
                              const char *path)
{
  (void)user;
  return zzplay_playlist_save_m3u(list, path);
}

static int rexx_quit_blocked(void *user)
{
  (void)user;
  return zzplay_requester_active();
}

static int rexx_show(void *user)
{
  (void)user;
  return zzplay_gui_show();
}

static void rexx_reply_rc(struct RexxMsg *msg, LONG rc, LONG rc2)
{
  msg->rm_Result1 = rc;
  msg->rm_Result2 = rc2;
  ReplyMsg(&msg->rm_Node);
}

static void rexx_reply_msg(struct RexxMsg *msg, const ZZPlayRexxReply *reply)
{
  UBYTE *result;

  if (reply->rc != ZZPLAY_REXX_RC_OK || !reply->has_result ||
      (msg->rm_Action & RXFF_RESULT) == 0) {
    rexx_reply_rc(msg, reply->rc, 0);
    return;
  }
  result = CreateArgstring((CONST_STRPTR)reply->result,
                           (ULONG)strlen(reply->result));
  if (!result) {
    rexx_reply_rc(msg, RC_ERROR, ERR10_003);
    return;
  }
  rexx_reply_rc(msg, RC_OK, (LONG)result);
}

/* Copy the command text into rexx_line. Anything that is not a command
 * string is answered here. Senders other than ARexx itself may pass a plain
 * C string instead of an argstring, so the length is not taken from the
 * argstring header. */
static int rexx_take_command(struct RexxMsg *msg)
{
  const char *text;
  size_t length = 0U;

  if ((msg->rm_Action & RXCODEMASK) != RXCOMM || !msg->rm_Args[0]) {
    rexx_reply_rc(msg, RC_ERROR, ERR10_010);
    return 0;
  }
  text = (const char *)msg->rm_Args[0];
  while (length < sizeof(rexx_line) && text[length] != '\0') {
    length++;
  }
  if (length == sizeof(rexx_line)) {
    rexx_reply_rc(msg, RC_ERROR, ERR10_009);
    return 0;
  }
  memcpy(rexx_line, text, length + 1U);
  return 1;
}

/* Run the held command once the gate opens. Returns 1 when it ran. */
static int rexx_run_held(void)
{
  ZZPlayController *ctl = rexx_host.ctl;

  if (ctl->modal == ZZPLAY_MODAL_NONE) {
    (void)zzplay_controller_begin_modal(ctl, ZZPLAY_MODAL_REXX);
  }
  if (zzplay_controller_modal_ready(ctl) != ZZPLAY_MODAL_REXX) {
    return 0;
  }
  zzplay_rexx_execute(&rexx_host, rexx_line, &rexx_reply);
  zzplay_controller_end_modal(ctl);
  rexx_reply_msg(rexx_held, &rexx_reply);
  rexx_held = 0;
  return 1;
}

int zzplay_rexx_open(const ZZPlayRexxContext *context)
{
  int suffix;

  if (rexx_port) {
    return 1;
  }
  if (!context || !context->controller || !context->playlist) {
    return 0;
  }
  RexxSysBase = (struct RxsLib *)OpenLibrary(
      (CONST_STRPTR)"rexxsyslib.library", 36U);
  if (!RexxSysBase) {
    return 0;
  }
  rexx_port = CreateMsgPort();
  if (!rexx_port) {
    zzplay_rexx_close();
    return 0;
  }

  /* Look up and publish the name in one Forbid, so two ZZPlays starting
   * together cannot both take it. */
  Forbid();
  for (suffix = 0; suffix <= ZZPLAY_REXX_MAX_SUFFIX; suffix++) {
    if (suffix == 0) {
      strcpy(rexx_port_name, ZZPLAY_REXX_BASE_NAME);
    } else {
      sprintf(rexx_port_name, "%s.%d", ZZPLAY_REXX_BASE_NAME, suffix);
    }
    if (!FindPort((CONST_STRPTR)rexx_port_name)) {
      break;
    }
  }
  if (suffix <= ZZPLAY_REXX_MAX_SUFFIX) {
    rexx_port->mp_Node.ln_Name = rexx_port_name;
    rexx_port->mp_Node.ln_Pri = 0;
    AddPort(rexx_port);
  }
  Permit();
  if (suffix > ZZPLAY_REXX_MAX_SUFFIX) {
    rexx_port->mp_Node.ln_Name = 0;
    zzplay_rexx_close();
    return 0;
  }

  memset(&rexx_host, 0, sizeof(rexx_host));
  rexx_host.ctl = context->controller;
  rexx_host.playlist = context->playlist;
  rexx_host.item_path = context->item_path;
  rexx_host.version = context->version;
  rexx_host.add_path = rexx_add_path;
  rexx_host.save_playlist = rexx_save_playlist;
  rexx_host.quit_blocked = rexx_quit_blocked;
  rexx_host.show = rexx_show;
  return 1;
}

void zzplay_rexx_close(void)
{
  struct RexxMsg *msg;

  if (rexx_port) {
    if (rexx_port->mp_Node.ln_Name) {
      /* Once the port is gone nobody can find it, so the queue drained
       * below is final. */
      Forbid();
      RemPort(rexx_port);
      Permit();
    }
    if (rexx_held) {
      if (rexx_host.ctl && rexx_host.ctl->modal == ZZPLAY_MODAL_REXX) {
        zzplay_controller_end_modal(rexx_host.ctl);
      }
      rexx_reply_rc(rexx_held, RC_ERROR, 0);
      rexx_held = 0;
    }
    while ((msg = (struct RexxMsg *)GetMsg(rexx_port)) != 0) {
      rexx_reply_rc(msg, RC_ERROR, 0);
    }
    rexx_port->mp_Node.ln_Name = 0;
    DeleteMsgPort(rexx_port);
    rexx_port = 0;
  }
  rexx_port_name[0] = '\0';
  if (RexxSysBase) {
    CloseLibrary((struct Library *)RexxSysBase);
    RexxSysBase = 0;
  }
}

const char *zzplay_rexx_port_name(void)
{
  return rexx_port ? rexx_port_name : 0;
}

uint32_t zzplay_rexx_signal_mask(void)
{
  return rexx_port ? (uint32_t)1UL << rexx_port->mp_SigBit : 0U;
}

void zzplay_rexx_poll(void)
{
  struct RexxMsg *msg;

  /* Look at the queue, not the signal: the idle Wait() has cleared it. */
  if (!rexx_port ||
      (!rexx_held && rexx_port->mp_MsgList.lh_Head->ln_Succ == 0)) {
    return;
  }
  if (rexx_held && !rexx_run_held()) {
    return;
  }
  while ((msg = (struct RexxMsg *)GetMsg(rexx_port)) != 0) {
    if (!rexx_take_command(msg)) {
      continue;
    }
    if (zzplay_rexx_needs_gate(rexx_line)) {
      rexx_held = msg;
      if (!rexx_run_held()) {
        return;
      }
      continue;
    }
    zzplay_rexx_execute(&rexx_host, rexx_line, &rexx_reply);
    rexx_reply_msg(msg, &rexx_reply);
  }
}
