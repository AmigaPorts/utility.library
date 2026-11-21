/* SPDX-License-Identifier: MPL-2.0 */
// Library entry stubs. Exec's jump table passes arguments in registers; the
// implementations are ordinary stack-ABI C functions. Each stub pushes the
// registers in reverse parameter order, calls the function and drops the
// arguments again. Results stay in D0 (and D1 for 64-bit values).
//
// The *_BASE variants also pass the library base from A6 as a trailing
// parameter, for functions that need Exec or library state.
//
// This file holds only assembly, so the compiler never emits code into a
// section these blocks have switched behind its back.
#include <exec/types.h>
#include <exec/execbase.h>

#include <stddef.h>

#define ENTRY_BEGIN(name) asm("\t.text\n\t.even\n\t.globl _" #name "Entry\n_" #name "Entry:")
#define ENTRY_PUSH(reg) asm("\tmove.l " #reg ",-(sp)")
#define ENTRY_END(name, bytes) asm("\tjsr _" #name "\n\tlea " #bytes "(sp),sp\n\trts")

#define ENTRY_1(name, r1) ENTRY_BEGIN(name); ENTRY_PUSH(r1); ENTRY_END(name, 4)
#define ENTRY_2(name, r1, r2) ENTRY_BEGIN(name); ENTRY_PUSH(r2); ENTRY_PUSH(r1); ENTRY_END(name, 8)
#define ENTRY_3(name, r1, r2, r3)                                                                                      \
    ENTRY_BEGIN(name); ENTRY_PUSH(r3); ENTRY_PUSH(r2); ENTRY_PUSH(r1); ENTRY_END(name, 12)

#define ENTRY_0_BASE(name) ENTRY_BEGIN(name); ENTRY_PUSH(a6); ENTRY_END(name, 4)
#define ENTRY_1_BASE(name, r1) ENTRY_BEGIN(name); ENTRY_PUSH(a6); ENTRY_PUSH(r1); ENTRY_END(name, 8)
#define ENTRY_2_BASE(name, r1, r2)                                                                                     \
    ENTRY_BEGIN(name); ENTRY_PUSH(a6); ENTRY_PUSH(r2); ENTRY_PUSH(r1); ENTRY_END(name, 12)
#define ENTRY_3_BASE(name, r1, r2, r3)                                                                                 \
    ENTRY_BEGIN(name); ENTRY_PUSH(a6); ENTRY_PUSH(r3); ENTRY_PUSH(r2); ENTRY_PUSH(r1); ENTRY_END(name, 16)

// The math entries are documented to work without the library base in A6 and
// to preserve every address register, including the usual scratch registers
// A0 and A1. A compiled C function may clobber those two, so save them here.
#define ENTRY_MATH(name)                                                                                               \
    ENTRY_BEGIN(name);                                                                                                 \
    asm("\tmove.l a0,-(sp)\n\tmove.l a1,-(sp)\n\tmove.l d1,-(sp)\n\tmove.l d0,-(sp)");                                 \
    asm("\tjsr _" #name "\n\taddq.l #8,sp\n\tmove.l (sp)+,a1\n\tmove.l (sp)+,a0\n\trts")

// The 64-bit multiplies return their product in different registers depending
// on the CPU, as the original does: a 68020 or later gives the exact product
// with the low half in D0 and the high half in D1 (the result of MULS.L or
// MULU.L into D1:D0), while a 68000 or 68010 goes through the software path in
// math.c, which returns the high half in D0. Exec records the CPU in AttnFlags
// at ExecBase+296, which is readable without A6 and without clobbering anything
// but the flags.
//
// The 68020 instruction is emitted as raw words so the file still assembles for
// a 68000 target: 0x4C01 is MULx.L D1,<Dh:Dl> and the extension word selects
// D0 as Dl, D1 as Dh, a 64-bit result, and signed (bit 11) or unsigned.
_Static_assert(offsetof(struct ExecBase, AttnFlags) == 296, "AttnFlags offset assumed by ENTRY_MATH64");

#define ENTRY_MATH64(name, extension)                                                                                  \
    ENTRY_BEGIN(name);                                                                                                 \
    asm("\tmove.l a0,-(sp)\n\tmovea.l 4.w,a0\n\tbtst #1,297(a0)\n\tmovea.l (sp)+,a0\n\tbeq.s 1f");                      \
    asm("\t.word 0x4C01," #extension "\n\trts\n1:");                                                               \
    asm("\tmove.l a0,-(sp)\n\tmove.l a1,-(sp)\n\tmove.l d1,-(sp)\n\tmove.l d0,-(sp)");                             \
    asm("\tjsr _" #name "\n\taddq.l #8,sp\n\tmove.l (sp)+,a1\n\tmove.l (sp)+,a0\n\trts")

// The 32-bit multiplies give the same truncated product on every CPU, so a
// 68020 or later may simply use MULS.L/MULU.L (0x4C01 is MULx.L D1,D0; the
// extension word selects D0 as the destination, a 32-bit result and signed
// or unsigned). Divisions stay in software: a native divide by zero would
// raise an exception where the software path returns a defined result.
#define ENTRY_MULT32(name, extension)                                                                                  \
    ENTRY_BEGIN(name);                                                                                                 \
    asm("\tmove.l a0,-(sp)\n\tmovea.l 4.w,a0\n\tbtst #1,297(a0)\n\tmovea.l (sp)+,a0\n\tbeq.s 1f");                      \
    asm("\t.word 0x4C01," #extension "\n\trts\n1:");                                                               \
    asm("\tmove.l a0,-(sp)\n\tmove.l a1,-(sp)\n\tmove.l d1,-(sp)\n\tmove.l d0,-(sp)");                             \
    asm("\tjsr _" #name "\n\taddq.l #8,sp\n\tmove.l (sp)+,a1\n\tmove.l (sp)+,a0\n\trts")

ENTRY_2(FindTagItem, d0, a0);
ENTRY_3(GetTagData, d0, d1, a0);
ENTRY_3(PackBoolTags, d0, a0, a1);
ENTRY_1(NextTagItem, a0);
ENTRY_3(FilterTagChanges, a0, a1, d0);
ENTRY_3(MapTags, a0, a1, d0);
ENTRY_1_BASE(AllocateTagItems, d0);
ENTRY_1_BASE(CloneTagItems, a0);
ENTRY_1_BASE(FreeTagItems, a0);
ENTRY_2(RefreshTagItemClones, a0, a1);
ENTRY_2(TagInArray, d0, a0);
ENTRY_3(FilterTagItems, a0, a1, d0);

// CallHookPkt(hook A0, object A2, message A1) uses the very registers a hook
// entry expects, so it only has to continue at hook->h_Entry (offset 8).
// Pushing that address and returning into it is a tail jump that modifies no
// register at all: the caller's non-scratch registers survive, and the hook
// returns straight to the caller with its result in D0.
ENTRY_BEGIN(CallHookPkt);
asm("\tmove.l 8(a0),-(sp)\n\trts");

ENTRY_2(Amiga2Date, d0, a0);
ENTRY_1(Date2Amiga, a0);
ENTRY_1(CheckDate, a0);
ENTRY_MULT32(SMult32, 0x0800);
ENTRY_MULT32(UMult32, 0x0000);
ENTRY_MATH(SDivMod32);
ENTRY_MATH(UDivMod32);
ENTRY_2(Stricmp, a0, a1);
ENTRY_3(Strnicmp, a0, a1, d0);
ENTRY_1(ToUpper, d0);
ENTRY_1(ToLower, d0);
ENTRY_2(ApplyTagChanges, a0, a1);
ENTRY_MATH64(SMult64, 0x0C01);
ENTRY_MATH64(UMult64, 0x0401);
ENTRY_3(PackStructureTags, a0, a1, a2);
ENTRY_3(UnpackStructureTags, a0, a1, a2);
ENTRY_2_BASE(AddNamedObject, a0, a1);
ENTRY_2_BASE(AllocNamedObjectA, a0, a1);
ENTRY_1_BASE(AttemptRemNamedObject, a0);
ENTRY_3_BASE(FindNamedObject, a0, a1, a2);
ENTRY_1_BASE(FreeNamedObject, a0);
ENTRY_1(NamedObjectName, a0);
ENTRY_1_BASE(ReleaseNamedObject, a0);
ENTRY_2_BASE(RemNamedObject, a0, a1);
ENTRY_0_BASE(GetUniqueID);
