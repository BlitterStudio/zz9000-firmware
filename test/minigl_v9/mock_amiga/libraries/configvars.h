/*
 * Mock AmigaOS libraries/configvars.h for ZZ9000 host transport E2E integration test
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef LIBRARIES_CONFIGVARS_H
#define LIBRARIES_CONFIGVARS_H

#include <exec/types.h>

struct ConfigDev {
    APTR  cd_BoardAddr;
    ULONG cd_BoardSize;
    UWORD cd_Rom_er_Manufacturer;
    UBYTE cd_Rom_er_Product;
};

#endif /* LIBRARIES_CONFIGVARS_H */
