/*
 * Mock AmigaOS devices/inputevent.h for the guest-client host harness.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef DEVICES_INPUTEVENT_H
#define DEVICES_INPUTEVENT_H

#include <exec/types.h>


#define IEQUALIFIER_LSHIFT     (1U << 0)
#define IEQUALIFIER_RSHIFT     (1U << 1)
#define IEQUALIFIER_LEFTBUTTON (1U << 12)
#define IEQUALIFIER_RBUTTON    (1U << 13)
#define IEQUALIFIER_MIDBUTTON  (1U << 14)
#define IECLASS_RAWKEY      1
#define IECLASS_RAWMOUSE    2

struct InputEvent {
    struct InputEvent *ie_NextEvent;
    UWORD              ie_Class;
    UWORD              ie_SubClass;
    UWORD              ie_Code;
    UWORD              ie_Qualifier;
};

#endif /* DEVICES_INPUTEVENT_H */
