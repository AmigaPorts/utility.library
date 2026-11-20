/* SPDX-License-Identifier: MPL-2.0 */
#include "internal/library.h"

// Provided by the linker script: the end of the module image.
extern const UBYTE rom_end;

// A library HUNK is not a Shell application. The linker keeps this entry first.
LONG __attribute__((used, section(".text.entry"))) doNotExecute(void)
{
    return -1;
}

const char deviceName[] = LIBRARY_NAME;
const char deviceIdString[] = VERSION_STRING;

const struct Resident RomTag __attribute__((used, aligned(2), section(".rodata.romtag"))) = {
    RTC_MATCHWORD,
    (struct Resident*)&RomTag,
    (APTR)&rom_end,
    RTF_COLDSTART,
    LIBRARY_VERSION,
    NT_LIBRARY,
    // Original utility priority: after Exec and Expansion, before every
    // module that opens the library during its own initialization.
    103,
    (char*)deviceName,
    (char*)deviceIdString,
    (APTR)Init,
};
