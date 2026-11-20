/* SPDX-License-Identifier: MPL-2.0 */
#include "internal/library.h"
#include "internal/namedobject.h"

#define __NOLIBBASE__
#include <proto/exec.h>

struct UtilityLibrary* utilityOpen(struct UtilityLibrary* base asm("a6"))
{
    base->utility.ub_LibNode.lib_Flags &= ~LIBF_DELEXP;
    base->utility.ub_LibNode.lib_OpenCnt++;

    return base;
}

ULONG utilityExpunge(struct UtilityLibrary* base asm("a6"))
{
    struct ExecBase* SysBase = base->exec;
    struct Library* library = &base->utility.ub_LibNode;

    // A ROM resident cannot be loaded again, so it has system lifetime.
    // Only a disk-loaded instance gives its memory and segment list back.
    if (!base->segment) {
        return 0;
    }

    library->lib_Flags |= LIBF_DELEXP;

    if (library->lib_OpenCnt != 0) {
        return 0;
    }

    Remove(&library->lib_Node);

    ULONG segment = base->segment;
    ULONG negative = library->lib_NegSize;

    FreeMem((UBYTE*)base - negative, negative + library->lib_PosSize);

    return segment;
}

ULONG utilityClose(struct UtilityLibrary* base asm("a6"))
{
    struct Library* library = &base->utility.ub_LibNode;

    if (--library->lib_OpenCnt != 0) {
        return 0;
    }

    if (library->lib_Flags & LIBF_DELEXP) {
        return utilityExpunge(base);
    }

    return 0;
}

ULONG utilityReserved(void)
{
    return 0;
}

// -108, -114 and -192 name no function in any public interface description.
// They stay callable and do nothing.
void utilityReservedSlot(void)
{
}

// Exec calls a non-AUTOINIT Resident with the segment list in A0, both at
// cold start (zero) and when ramlib loads the module from LIBS:.
struct UtilityLibrary* Init(ULONG segment asm("a0"), struct ExecBase* SysBase asm("a6"))
{
    struct UtilityLibrary* base =
        (APTR)MakeLibrary((APTR)utilityFunctions, NULL, NULL, sizeof(struct UtilityLibrary), 0);

    if (!base) {
        return NULL;
    }

    struct Library* library = &base->utility.ub_LibNode;

    base->exec = SysBase;
    base->segment = segment;

    // OpenLibrary() compares its version argument with lib_Version, and a
    // disk library is only found if ln_Name equals its file name.
    library->lib_Node.ln_Type = NT_LIBRARY;
    library->lib_Node.ln_Name = (char*)deviceName;
    library->lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    library->lib_Version = LIBRARY_VERSION;
    library->lib_Revision = LIBRARY_REVISION;
    library->lib_IdString = (STRPTR)deviceIdString;

    NamedObjectsLibInit(base);

    AddLibrary(library);

    return base;
}
