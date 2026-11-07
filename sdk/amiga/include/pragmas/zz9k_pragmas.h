/* SAS/C-style app-client pragmas for zz9k.library. */
#ifndef PRAGMAS_ZZ9K_PRAGMAS_H
#define PRAGMAS_ZZ9K_PRAGMAS_H

#ifndef CLIB_ZZ9K_PROTOS_H
#include <clib/zz9k_protos.h>
#endif

#ifndef __GNUC__
#pragma libcall ZZ9KBase ZZ9KAppOpen          162 9802
#pragma libcall ZZ9KBase ZZ9KAppLoadFile      168 9802
#pragma libcall ZZ9KBase ZZ9KAppLoadMemory    16E 09803
#pragma libcall ZZ9KBase ZZ9KAppStart         174 09803
#pragma libcall ZZ9KBase ZZ9KAppStop          17A 801
#pragma libcall ZZ9KBase ZZ9KAppStatus        180 9802
#pragma libcall ZZ9KBase ZZ9KAppSendMessage   186 9802
#pragma libcall ZZ9KBase ZZ9KAppGetMessage    18C 9802
#pragma libcall ZZ9KBase ZZ9KAppMessageSignal 192 801
#pragma libcall ZZ9KBase ZZ9KAppClose         198 801
#endif
#endif
