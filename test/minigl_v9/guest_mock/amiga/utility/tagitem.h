/*
 * Mock AmigaOS utility/tagitem.h for the guest-client host harness.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef UTILITY_TAGITEM_H
#define UTILITY_TAGITEM_H

#include <exec/types.h>

typedef ULONG Tag;

#define TAG_DONE   ((Tag)0)
#define TAG_END    ((Tag)0)
#define TAG_IGNORE ((Tag)1)
#define TAG_MORE   ((Tag)2)
#define TAG_SKIP   ((Tag)3)

#define TAG_USER   ((Tag)(1L << 31))

struct TagItem {
    Tag   ti_Tag;
    ULONG ti_Data;
};

#endif /* UTILITY_TAGITEM_H */
