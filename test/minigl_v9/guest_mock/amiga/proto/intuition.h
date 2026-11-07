/*
 * Mock AmigaOS proto/intuition.h for the guest-client host harness.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PROTO_INTUITION_H
#define PROTO_INTUITION_H

#include <exec/types.h>
#include <exec/libraries.h>
#include <intuition/intuition.h>
#include <intuition/intuitionbase.h>
#include <intuition/screens.h>

extern struct IntuitionBase *IntuitionBase;

#endif /* PROTO_INTUITION_H */
