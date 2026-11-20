/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <exec/types.h>

ULONG ToUpper(UBYTE ch);
ULONG ToLower(UBYTE ch);
LONG Stricmp(STRPTR string1, STRPTR string2);
LONG Strnicmp(STRPTR string1, STRPTR string2, LONG length);
