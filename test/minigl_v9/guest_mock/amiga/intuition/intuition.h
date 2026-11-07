/*
 * Mock AmigaOS intuition/intuition.h for the guest-client host harness.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef INTUITION_INTUITION_H
#define INTUITION_INTUITION_H

#include <exec/types.h>
#include <exec/ports.h>
#include <graphics/gfx.h>
#include <graphics/clip.h>
#include <utility/tagitem.h>

/* Window attribute tags */
#define WA_Left        (TAG_USER + 1)
#define WA_Top         (TAG_USER + 2)
#define WA_Width       (TAG_USER + 3)
#define WA_Height      (TAG_USER + 4)
#define WA_InnerWidth  (TAG_USER + 29)
#define WA_InnerHeight (TAG_USER + 30)
#define WA_IDCMP       (TAG_USER + 7)
#define WA_Flags       (TAG_USER + 9)
#define WA_CustomScreen (TAG_USER + 26)
#define WA_Title       (TAG_USER + 11)
#define WA_Backdrop    (TAG_USER + 13)
#define WA_Borderless  (TAG_USER + 14)
#define WA_Activate    (TAG_USER + 16)
#define WA_SmartRefresh (TAG_USER + 33)

/* Window flags */
#define WFLG_BORDERLESS    (1UL << 11)
#define WFLG_SMART_REFRESH (1UL << 8)
#define WFLG_DEPTHGADGET   (1UL << 6)
#define WFLG_DRAGBAR       (1UL << 5)
#define WFLG_CLOSEGADGET   (1UL << 4)

/* IDCMP flag set */
#define IDCMP_VANILLAKEY   (1UL << 21)
#define IDCMP_RAWKEY       (1UL << 20)
#define IDCMP_MOUSEMOVE    (1UL << 10)
#define IDCMP_MOUSEBUTTONS (1UL << 9)
#define IDCMP_DELTAMOVE    (1UL << 22)
#define IDCMP_SIZEVERIFY   (1UL << 14)
#define IDCMP_MENUVERIFY   (1UL << 17)
#define IDCMP_REQVERIFY    (1UL << 16)
#define IDCMP_REFRESHWINDOW (1UL << 18)

struct MsgPort;

struct Window {
    struct Window *NextWindow;
    WORD           LeftEdge;
    WORD           TopEdge;
    WORD           Width;
    WORD           Height;
    WORD           MouseY;
    WORD           MouseX;
    WORD           BorderLeft;
    WORD           BorderTop;
    WORD           BorderRight;
    WORD           BorderBottom;
    struct RastPort *RPort;
    void          *UserPort;
    void          *WScreen;   /* struct Screen * */
    ULONG          IDCMPFlags;
};

struct IntuiMessage {
    struct Message  ExecMessage;
    ULONG           Class;
    UWORD           Code;
    UWORD           Qualifier;
    WORD            MouseX;
    WORD            MouseY;
};

struct Window *OpenWindowTags(void *newwin, Tag tag1, ...);
void CloseWindow(struct Window *window);
void ClearPointer(struct Window *window);
void SetPointer(struct Window *window, UWORD *sprite, LONG width, LONG height,
                LONG xoffset, LONG yoffset);
BOOL ModifyIDCMP(struct Window *window, ULONG flags);
#endif /* INTUITION_INTUITION_H */
