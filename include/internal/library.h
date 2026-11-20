/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/libraries.h>
#include <exec/lists.h>
#include <exec/resident.h>
#include <exec/semaphores.h>
#include <utility/utility.h>

// The public base remains the original ABI prefix. All mutable state lives
// here rather than in .data/.bss, so the module can run from ROM.
struct UtilityLibrary {
    struct UtilityBase utility;
    struct ExecBase* exec;
    // The NULL ("root") name space of the NamedObject functions.
    struct List root_namespace;
    struct SignalSemaphore named_object_semaphore;
    ULONG unique_id;
};

extern const char deviceName[];
extern const char deviceIdString[];
extern const struct Resident RomTag;
// Includes the four lifecycle vectors; the last V40 vector is -270.
#define UTILITY_VECTOR_COUNT 45
extern const APTR utilityFunctions[UTILITY_VECTOR_COUNT + 1];

struct UtilityLibrary* Init(ULONG segment asm("a0"), struct ExecBase* SysBase asm("a6"));
struct UtilityLibrary* utilityOpen(struct UtilityLibrary* base asm("a6"));
ULONG utilityClose(struct UtilityLibrary* base asm("a6"));
ULONG utilityExpunge(void);
ULONG utilityReserved(void);
void utilityReservedSlot(void);
