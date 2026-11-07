/*
 * Mock AmigaOS intuition/screens.h for the guest-client host harness.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef INTUITION_SCREENS_H
#define INTUITION_SCREENS_H

#include <exec/types.h>
#include <exec/ports.h>
#include <graphics/gfx.h>
#include <graphics/displayinfo.h>
#include <intuition/intuition.h>
#include <utility/tagitem.h>

#define STDSCREENWIDTH  ~0
#define STDSCREENHEIGHT ~0

/* Screen attribute tags */
#define SA_DisplayID  (TAG_USER + 41)
#define SA_Width      (TAG_USER + 34)
#define SA_Height     (TAG_USER + 35)
#define SA_Depth      (TAG_USER + 36)
#define SA_Quiet      (TAG_USER + 38)
#define SA_ShowTitle (TAG_USER + 33)

struct Screen {
    struct Screen *NextScreen;
    WORD            LeftEdge;
    WORD            TopEdge;
    WORD            Width;
    WORD            Height;
    struct RastPort RastPort;
    struct ViewPort ViewPort;
    UBYTE           TitlePad;
};

struct Screen *OpenScreenTags(void *newscreen, Tag tag1, ...);
void CloseScreen(struct Screen *screen);
struct Screen *LockPubScreen(STRPTR name);
void UnlockPubScreen(void *unused, struct Screen *screen);
void ScreenToFront(struct Screen *screen);
BOOL OpenWorkBench(void);
BOOL CloseWorkBench(void);

#endif /* INTUITION_SCREENS_H */
