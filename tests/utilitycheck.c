/* SPDX-License-Identifier: MPL-2.0 */
/* utilitycheck.c -- differential test for this project's from-scratch
 * utility.library (src/) against the real
 * ROM-resident utility.library, for all 11 tag-list functions implemented
 * so far (FindTagItem, GetTagData, NextTagItem, TagInArray,
 * AllocateTagItems, FreeTagItems, CloneTagItems, RefreshTagItemClones,
 * PackBoolTags, FilterTagChanges, MapTags).
 *
 * Opens BOTH library bases explicitly:
 *   - "utility-new.library": our own file, under a deliberately
 *     non-colliding disk name (see HEADERS.md's own
 *     header comment and this project's harness docs for why --
 *     OpenLibrary("utility.library", ...) on a real V40 host always
 *     resolves to the ROM-resident copy via exec's in-memory library
 *     list, never to a same-named disk file, with no overlay loader in
 *     place yet).
 *   - "utility.library": the real ROM-resident copy, found directly by
 *     name, no LIBS:/assign involvement.
 *
 * Standard NDK/libnix inline-stub protos assume a single fixed global
 * base variable (e.g. UtilityBase), which doesn't work when the same
 * process needs to call through TWO different bases in one run. Instead,
 * each of the 8 functions gets a small hand-written call stub
 * (CALLSTUB_*() below) that takes an explicit struct Library* and binds
 * it to A6 itself, then loads the other arguments into the exact
 * registers HEADERS.md's own LVO-table comment block
 * documents (cross-checked there against the NDK autodocs already).
 * This is the direct inverse of src/entry.c's
 * LIBSTUB_n() macros (those build a library's entry points *from* a
 * stack-ABI C function; these *call into* a library at a chosen base
 * from ordinary stack-ABI C code) -- same underlying technique (GCC
 * file-scope asm() trampolines), opposite direction.
 *
 * Reporting: PASS/FAIL per function/sub-case, via kprint.h's kprintf()
 * (serial.device -- this project's existing test-binary convention, see
 * tools/probe/libcheck.c) which is what golden.py's real-rig capture
 * actually diffs. ALSO mirrored to Output() via dos.library's ordinary
 * Write() -- volamos (the fast-local-iteration tool, see
 * tools/README.md) doesn't emulate serial.device at all, but does
 * support plain dos.library Output() writes straight to host stdout, so
 * this one binary works for both the instant local loop and the real
 * golden-test capture without any conditional compilation.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <exec/nodes.h>
#include <utility/tagitem.h>
#include <utility/date.h>
#include <utility/hooks.h>
#include <utility/pack.h>
#include <utility/name.h>
#include <exec/ports.h>
#include <exec/io.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <clib/alib_protos.h>

#include "kprint.h"

static const char __attribute__((used)) __verstag[] =
    "\0$VER: utilitycheck 1.0 (11.10.26)";

/* ======================================================================
 * Reporting: kprintf() (serial, authoritative for the golden-test
 * capture) mirrored to Write(Output(), ...) (volamos-visible). The
 * RawDoFmt()-into-a-buffer dance here is the same technique kprint.c
 * itself uses internally -- duplicated locally rather than exposing
 * kprint.c's private StuffChar/StuffBuf, to avoid touching that file.
 * ====================================================================== */

struct ReportBuf { UBYTE *pos; UBYTE *end; };

static void ReportStuffChar(register UBYTE ch __asm("d0"),
                             register struct ReportBuf *buf __asm("a3"))
{
    if (buf->pos < buf->end)
        *buf->pos++ = ch;
}

static void report(CONST_STRPTR fmt, APTR args)
{
    UBYTE line[256];
    struct ReportBuf buf;

    kprintf(fmt, args);

    buf.pos = line;
    buf.end = line + sizeof(line) - 1;
    RawDoFmt(fmt, args, (void (*)())ReportStuffChar, &buf);
    Write(Output(), line, (LONG)(buf.pos - line));
}

static ULONG g_pass = 0, g_fail = 0;

static void result(CONST_STRPTR name, BOOL ok)
{
    ULONG args[1];
    args[0] = (ULONG)(APTR)name;

    if (ok) {
        g_pass++;
        report((CONST_STRPTR) "PASS %s\r\n", args);
    } else {
        g_fail++;
        report((CONST_STRPTR) "FAIL %s\r\n", args);
    }
}

/* ======================================================================
 * Call stubs: load an explicit struct Library* into A6, load the other
 * arguments into the registers HEADERS.md's vector table documents, JSR to
 * the fixed negative offset, RTS. No prologue/epilogue (matches
 * entry.c's ENTRY_n() convention): right after JSR lands here, the
 * return address is at 0(sp) and the stack-ABI arguments this function
 * was called with sit at 4(sp), 8(sp), 12(sp), ... in left-to-right
 * order -- every argument here is a 4-byte pointer or ULONG, so the
 * offsets step by exactly 4. The library function's own return value
 * (if any) comes back in D0, which m68k-gcc's normal calling convention
 * already treats as this function's return register -- nothing extra to
 * do.
 *
 * a6 MUST be saved and restored here, unlike entry.c's ENTRY_n()
 * entry trampolines (which deliberately clobber it freely): this was
 * confirmed the hard way, via volamos (see the PR notes) -- m68k-gcc's
 * calling convention on this target treats a6 as callee-saved for an
 * ordinary compiled C function, and *will* hoist a loop-invariant
 * function address into a6 across repeated calls (confirmed by
 * disassembling the compiled test program: a `while` loop calling
 * NextTagItem_Call() got compiled as "lea NextTagItem_Call(pc),a6" once
 * before the loop, then "jsr (a6)" each iteration) -- trusting the
 * callee not to repurpose a6 as a scratch register. A first version of
 * these macros clobbered a6 with the library base and never restored
 * it, which corrupted exactly that cached address on the second loop
 * iteration and crashed. entry.c's ENTRY_n() entries get away
 * without this because they're never *called* as an ordinary C
 * function by GCC-generated code -- they're only ever reached via a
 * JSR through a library's own jump table, a context GCC's register
 * allocator has no visibility into and makes no preservation
 * assumptions about.
 * ====================================================================== */

#define CALLSTUB_D0(name, offset) \
    extern ULONG name##_Call(struct Library *base, ULONG a1); \
    asm(".text"); \
    asm(".globl _" #name "_Call"); \
    asm("_" #name "_Call:"); \
    asm("\tmove.l a6,-(sp)"); \
    asm("\tmovea.l 8(sp),a6"); \
    asm("\tmove.l 12(sp),d0"); \
    asm("\tjsr " #offset "(a6)"); \
    asm("\tmovea.l (sp)+,a6"); \
    asm("\trts")

#define CALLSTUB_A0(name, offset) \
    extern ULONG name##_Call(struct Library *base, APTR a1); \
    asm(".text"); \
    asm(".globl _" #name "_Call"); \
    asm("_" #name "_Call:"); \
    asm("\tmove.l a6,-(sp)"); \
    asm("\tmovea.l 8(sp),a6"); \
    asm("\tmovea.l 12(sp),a0"); \
    asm("\tjsr " #offset "(a6)"); \
    asm("\tmovea.l (sp)+,a6"); \
    asm("\trts")

#define CALLSTUB_D0A0(name, offset) \
    extern ULONG name##_Call(struct Library *base, ULONG d0v, APTR a0v); \
    asm(".text"); \
    asm(".globl _" #name "_Call"); \
    asm("_" #name "_Call:"); \
    asm("\tmove.l a6,-(sp)"); \
    asm("\tmovea.l 8(sp),a6"); \
    asm("\tmove.l 12(sp),d0"); \
    asm("\tmovea.l 16(sp),a0"); \
    asm("\tjsr " #offset "(a6)"); \
    asm("\tmovea.l (sp)+,a6"); \
    asm("\trts")

#define CALLSTUB_D0D1A0(name, offset) \
    extern ULONG name##_Call(struct Library *base, ULONG d0v, ULONG d1v, APTR a0v); \
    asm(".text"); \
    asm(".globl _" #name "_Call"); \
    asm("_" #name "_Call:"); \
    asm("\tmove.l a6,-(sp)"); \
    asm("\tmovea.l 8(sp),a6"); \
    asm("\tmove.l 12(sp),d0"); \
    asm("\tmove.l 16(sp),d1"); \
    asm("\tmovea.l 20(sp),a0"); \
    asm("\tjsr " #offset "(a6)"); \
    asm("\tmovea.l (sp)+,a6"); \
    asm("\trts")

#define CALLSTUB_A0A1(name, offset) \
    extern ULONG name##_Call(struct Library *base, APTR a0v, APTR a1v); \
    asm(".text"); \
    asm(".globl _" #name "_Call"); \
    asm("_" #name "_Call:"); \
    asm("\tmove.l a6,-(sp)"); \
    asm("\tmovea.l 8(sp),a6"); \
    asm("\tmovea.l 12(sp),a0"); \
    asm("\tmovea.l 16(sp),a1"); \
    asm("\tjsr " #offset "(a6)"); \
    asm("\tmovea.l (sp)+,a6"); \
    asm("\trts")

#define CALLSTUB_D0A0A1(name, offset) \
    extern ULONG name##_Call(struct Library *base, ULONG d0v, APTR a0v, APTR a1v); \
    asm(".text"); \
    asm(".globl _" #name "_Call"); \
    asm("_" #name "_Call:"); \
    asm("\tmove.l a6,-(sp)"); \
    asm("\tmovea.l 8(sp),a6"); \
    asm("\tmove.l 12(sp),d0"); \
    asm("\tmovea.l 16(sp),a0"); \
    asm("\tmovea.l 20(sp),a1"); \
    asm("\tjsr " #offset "(a6)"); \
    asm("\tmovea.l (sp)+,a6"); \
    asm("\trts")

#define CALLSTUB_A0A1D0(name, offset) \
    extern ULONG name##_Call(struct Library *base, APTR a0v, APTR a1v, ULONG d0v); \
    asm(".text"); \
    asm(".globl _" #name "_Call"); \
    asm("_" #name "_Call:"); \
    asm("\tmove.l a6,-(sp)"); \
    asm("\tmovea.l 8(sp),a6"); \
    asm("\tmovea.l 12(sp),a0"); \
    asm("\tmovea.l 16(sp),a1"); \
    asm("\tmove.l 20(sp),d0"); \
    asm("\tjsr " #offset "(a6)"); \
    asm("\tmovea.l (sp)+,a6"); \
    asm("\trts")

/* CALLSTUB_A0A2A1(name, offset) -- call stub for CallHookPkt's own LVO
 * register assignment (A0=hook, A2=object, A1=message -- NOT the natural
 * left-to-right C parameter order, per <utility/hooks.h>'s own "register
 * natural order differs from this convention for C parameter order,
 * which is A0,A2,A1" note). A2 must be saved/restored here, unlike A0/A1:
 * A2-A6/D2-D7 are the callee-saved register set for an ordinary compiled
 * C function under this project's build target (same convention that
 * made A6-clobbering a real bug the first time this project wrote a
 * client-side call stub -- see this file's own big comment above, and
 * provenance/utility-library-tags.md's "Errors and fixes" -- and the
 * same reasoning CALLSTUB_MATH32 below already applies to its own A2
 * out-pointer). A0/A1 need no save/restore here: they're loaded fresh
 * from this stub's own arguments immediately before the JSR, and nothing
 * in this function needs their incoming values preserved across the
 * call.
 *
 * A3 ALSO needs saving/restoring here -- found the hard way, by this
 * work package's own first real-rig recording attempt (see "Errors and
 * fixes" in provenance/utility-library-callhook.md for the full trail):
 * entry.c's CallHookPkt trampoline (originally) uses A3 as its scratch
 * register to hold hook->h_Entry before the JSR into it, and -- entirely
 * correctly for an LVO entry point, which this project's own convention
 * already documents as free to clobber anything (see entry.c's
 * comment) -- never restores it. But CallHookPkt_Call() here, unlike the
 * library's own entry stub, IS called as an ordinary GCC-compiled C
 * function from this test program's own code, which may be relying on
 * A3 (a callee-saved register under the normal convention) still
 * holding whatever it held before the call. Omitting A3 from this
 * stub's own save/restore reproduced exactly the same class of bug this
 * project's very first work package found with A6 (provenance/
 * utility-library-tags.md) -- except here it corrupted several
 * unrelated LATER test cases' own cached register state instead of an
 * immediately following iteration, which is why the first recording
 * attempt's failures (Amiga2Date/Date2Amiga/CheckDate-InvalidMdayRejected/
 * all four math functions) looked unrelated to CallHookPkt at a glance. */
#define CALLSTUB_A0A2A1(name, offset) \
    extern ULONG name##_Call(struct Library *base, APTR a0v, APTR a2v, APTR a1v); \
    asm(".text"); \
    asm(".globl _" #name "_Call"); \
    asm("_" #name "_Call:"); \
    asm("\tmove.l a6,-(sp)"); \
    asm("\tmove.l a3,-(sp)"); \
    asm("\tmove.l a2,-(sp)"); \
    asm("\tmovea.l 16(sp),a6"); \
    asm("\tmovea.l 20(sp),a0"); \
    asm("\tmovea.l 24(sp),a2"); \
    asm("\tmovea.l 28(sp),a1"); \
    asm("\tjsr " #offset "(a6)"); \
    asm("\tmovea.l (sp)+,a2"); \
    asm("\tmovea.l (sp)+,a3"); \
    asm("\tmovea.l (sp)+,a6"); \
    asm("\trts")

/* CALLSTUB_A0A2A1_A3PROBE(name, offset) -- A3-preservation oracle for
 * CallHookPkt, added as part of this work package's dated correction
 * (provenance/utility-library-callhook.md's "2026-10-13 correction"
 * section): CALLSTUB_A0A2A1 above already has to save/restore A3 around
 * the JSR purely for its OWN correctness (A3 is callee-saved for this
 * test program's own compiled C code, and the library's entry point --
 * before this correction -- clobbered it), but that save/restore
 * protects THIS test program from the library, it does not PROVE
 * anything about whether the library itself preserves A3 -- the exact
 * same blind spot that let the original CallHookPkt_Entry bug ship
 * without being caught. This stub instead *exposes* A3's observed
 * post-call value for inspection, the same technique CALLSTUB_MATH32
 * below already uses for A0/A1 (load a recognizable sentinel
 * immediately before the JSR, read back whatever is actually there
 * immediately after, let the caller compare): it loads HOOK_SENTINEL_A3
 * into A3 right before the JSR (deliberately AFTER the real
 * hook/object/message arguments are already loaded, since A3 is not one
 * of CallHookPkt's own three parameter registers and this sentinel load
 * must not disturb them), then -- since A3 itself is the value under
 * test and so cannot also serve as this stub's own out-pointer register
 * the way CALLSTUB_MATH32 uses A2 for that role -- reloads the 5th
 * stack argument (the struct HookProbeResult* out-pointer) into A2
 * immediately after the JSR returns (A2's own incoming value, the
 * "object" parameter, is no longer needed by this point, so clobbering
 * it here is harmless) and writes D0 (CallHookPkt's own return value)
 * and the observed A3 through it, before finally restoring the real
 * saved A2/A3/A6 for this stub's own caller. */
struct HookProbeResult {
    ULONG d0;
    ULONG a3;
};

#define HOOK_SENTINEL_A3 0x41334133UL  /* "A3A3"-ish, matches the MATH_
                                         * SENTINEL_A0/A1 naming style */

#define CALLSTUB_A0A2A1_A3PROBE(name, offset) \
    extern void name##_CallA3Probe(struct Library *base, APTR a0v, APTR a2v, APTR a1v, struct HookProbeResult *out); \
    asm(".text"); \
    asm(".globl _" #name "_CallA3Probe"); \
    asm("_" #name "_CallA3Probe:"); \
    asm("\tmove.l a6,-(sp)"); \
    asm("\tmove.l a3,-(sp)"); \
    asm("\tmove.l a2,-(sp)"); \
    asm("\tmovea.l 16(sp),a6"); \
    asm("\tmovea.l 20(sp),a0"); \
    asm("\tmovea.l 24(sp),a2"); \
    asm("\tmovea.l 28(sp),a1"); \
    asm("\tmove.l #0x41334133,a3"); \
    asm("\tjsr " #offset "(a6)"); \
    asm("\tmovea.l 32(sp),a2"); \
    asm("\tmove.l d0,(a2)"); \
    asm("\tmove.l a3,4(a2)"); \
    asm("\tmovea.l (sp)+,a2"); \
    asm("\tmovea.l (sp)+,a3"); \
    asm("\tmovea.l (sp)+,a6"); \
    asm("\trts")

CALLSTUB_A0A2A1_A3PROBE(CallHookPkt, -102);

/* Offsets from HEADERS.md's own LVO table comment. */
CALLSTUB_D0A0(FindTagItem, -30);
CALLSTUB_D0D1A0(GetTagData, -36);
CALLSTUB_D0A0A1(PackBoolTags, -42);
CALLSTUB_A0(NextTagItem, -48);
CALLSTUB_A0A1D0(FilterTagChanges, -54);
CALLSTUB_A0A1D0(MapTags, -60);
CALLSTUB_D0(AllocateTagItems, -66);
CALLSTUB_A0(CloneTagItems, -72);
CALLSTUB_A0(FreeTagItems, -78);
CALLSTUB_A0A1(RefreshTagItemClones, -84);
CALLSTUB_D0A0(TagInArray, -90);

/* Fourth work package: FilterTagItems (-96), Amiga2Date (-120),
 * Date2Amiga (-126), CheckDate (-132). Reuses the exact same macro
 * shapes already defined above for the matching register signatures
 * (A0,A1,D0 / D0,A0 / A0) -- Amiga2Date's own VOID return is harmless
 * to call through the ULONG-returning CALLSTUB_D0A0 shape below (the
 * returned D0 value is simply never read by the caller). */
CALLSTUB_A0A1D0(FilterTagItems, -96);
CALLSTUB_A0A2A1(CallHookPkt, -102);
CALLSTUB_D0A0(Amiga2Date, -120);
CALLSTUB_A0(Date2Amiga, -126);
CALLSTUB_A0(CheckDate, -132);

/* ======================================================================
 * CALLSTUB_MATH32(name, offset) -- call stub for the four 32-bit math
 * functions (LVO -138/-144/-150/-156), which this project's task spec
 * says use a deliberately non-standard calling convention (per
 * utility.doc's own NOTES, quoted verbatim in src/math.c): A6 need not hold the library base internally,
 * and ALL address registers including A0/A1 are preserved across the
 * call. This stub both EXERCISES that claim empirically (loads A0/A1
 * with recognizable sentinel values immediately before the JSR, and
 * reports their values immediately after, alongside the D0/D1 result)
 * and still has to set A6 up correctly for the JSR itself -- the
 * "A6 can contain anything" note describes what the CALLEE requires,
 * not how the JMP-table dispatch mechanism works; JSR offset(A6) still
 * needs A6 == the chosen library base to compute the right jump-table
 * slot address, same as any other LVO call. A6 is saved/restored around
 * the call like every other CALLSTUB_* here (ordinary m68k-gcc callee-
 * saved convention for an outer caller function, same reasoning as
 * those macros).
 *
 * Output is written through an explicit struct pointer (out) rather
 * than returned, because a single C function can expose at most D0:D1
 * as a return value and this test needs FOUR results (D0, D1, A0, A1)
 * back from one call -- A2 holds the out-pointer across the JSR since
 * A0/A1 are occupied by the deliberate sentinels and the autodoc's own
 * "preserve ALL address registers" wording (not just "A0 and A1" as an
 * exhaustive list) implies A2 is just as safe to carry a live value
 * across the call as A0/A1 are.
 * ====================================================================== */

struct MathResult {
    ULONG d0;
    ULONG d1;
    ULONG a0;
    ULONG a1;
};

#define MATH_SENTINEL_A0 0x41304130UL  /* "A0A0"-ish, easy to eyeball */
#define MATH_SENTINEL_A1 0x41314131UL  /* "A1A1"-ish */

#define CALLSTUB_MATH32(name, offset) \
    extern void name##_Call(struct Library *base, ULONG d0v, ULONG d1v, struct MathResult *out); \
    asm(".text"); \
    asm(".globl _" #name "_Call"); \
    asm("_" #name "_Call:"); \
    asm("\tmove.l a6,-(sp)"); \
    asm("\tmove.l a2,-(sp)"); \
    asm("\tmovea.l 12(sp),a6"); \
    asm("\tmove.l 16(sp),d0"); \
    asm("\tmove.l 20(sp),d1"); \
    asm("\tmovea.l 24(sp),a2"); \
    asm("\tmove.l #0x41304130,a0"); \
    asm("\tmove.l #0x41314131,a1"); \
    asm("\tjsr " #offset "(a6)"); \
    asm("\tmove.l d0,(a2)"); \
    asm("\tmove.l d1,4(a2)"); \
    asm("\tmove.l a0,8(a2)"); \
    asm("\tmove.l a1,12(a2)"); \
    asm("\tmovea.l (sp)+,a2"); \
    asm("\tmovea.l (sp)+,a6"); \
    asm("\trts")

CALLSTUB_MATH32(SMult32, -138);
CALLSTUB_MATH32(UMult32, -144);
CALLSTUB_MATH32(SDivMod32, -150);
CALLSTUB_MATH32(UDivMod32, -156);

/* Seventh work package (provenance/utility-library-math64.md): SMult64
 * (-198), UMult64 (-204). Same CALLSTUB_MATH32 shape reused unmodified --
 * both functions share the exact same documented non-standard calling
 * convention (A6-independence, A0/A1 preservation) and D0,D1-in register
 * shape as the four 32-bit math functions above; only the *meaning* of
 * the D0:D1 output differs (a genuine 64-bit product here, vs
 * quotient:remainder for SDivMod32/UDivMod32 -- the macro doesn't care
 * either way, since it just reports D0/D1/A0/A1 verbatim). */
CALLSTUB_MATH32(SMult64, -198);
CALLSTUB_MATH32(UMult64, -204);

/* Sixth work package (provenance/utility-library-stringops.md): Stricmp
 * (-162), Strnicmp (-168), ToUpper (-174), ToLower (-180), ApplyTagChanges
 * (-186). All five reuse existing macro shapes already defined above for
 * the matching register signatures (A0,A1 / A0,A1,D0 / D0) -- none of
 * these functions carries the math32 family's non-standard calling
 * convention. */
CALLSTUB_A0A1(Stricmp, -162);
CALLSTUB_A0A1D0(Strnicmp, -168);
CALLSTUB_D0(ToUpper, -174);
CALLSTUB_D0(ToLower, -180);
CALLSTUB_A0A1(ApplyTagChanges, -186);

/* CALLSTUB_A0A1A2(name, offset) -- call stub for PackStructureTags/
 * UnpackStructureTags's own LVO register assignment (A0=pack,
 * A1=packTable, A2=tagList -> D0=num), eighth work package (see
 * provenance/utility-library-packtags.md). A2 must be saved/restored
 * here, like CALLSTUB_A0A2A1's own A2 handling above (A2-A6/D2-D7 are
 * this project's build target's callee-saved set for an ordinary
 * compiled C function). */
#define CALLSTUB_A0A1A2(name, offset) \
    extern ULONG name##_Call(struct Library *base, APTR a0v, APTR a1v, APTR a2v); \
    asm(".text"); \
    asm(".globl _" #name "_Call"); \
    asm("_" #name "_Call:"); \
    asm("\tmove.l a6,-(sp)"); \
    asm("\tmove.l a2,-(sp)"); \
    asm("\tmovea.l 12(sp),a6"); \
    asm("\tmovea.l 16(sp),a0"); \
    asm("\tmovea.l 20(sp),a1"); \
    asm("\tmovea.l 24(sp),a2"); \
    asm("\tjsr " #offset "(a6)"); \
    asm("\tmovea.l (sp)+,a2"); \
    asm("\tmovea.l (sp)+,a6"); \
    asm("\trts")

/* Eighth work package: PackStructureTags (-210), UnpackStructureTags
 * (-216). */
CALLSTUB_A0A1A2(PackStructureTags, -210);
CALLSTUB_A0A1A2(UnpackStructureTags, -216);

/* CALLSTUB_NONE(name, offset) -- call stub for a zero-argument LVO
 * (GetUniqueID, -270, ninth work package). No arguments to load at all;
 * still needs to set A6 to the chosen library base before the JSR, same
 * as every other stub here. */
#define CALLSTUB_NONE(name, offset) \
    extern ULONG name##_Call(struct Library *base); \
    asm(".text"); \
    asm(".globl _" #name "_Call"); \
    asm("_" #name "_Call:"); \
    asm("\tmove.l a6,-(sp)"); \
    asm("\tmovea.l 8(sp),a6"); \
    asm("\tjsr " #offset "(a6)"); \
    asm("\tmovea.l (sp)+,a6"); \
    asm("\trts")

/* Ninth work package (provenance/utility-library-namedobject.md): the
 * full V39 NamedObject family, -222 through -270. AddNamedObject/
 * AllocNamedObjectA/RemNamedObject all share the plain A0,A1 shape
 * (CALLSTUB_A0A1, already defined above); AttemptRemNamedObject/
 * FreeNamedObject/NamedObjectName/ReleaseNamedObject share the plain
 * single-A0 shape (CALLSTUB_A0, already defined above); FindNamedObject
 * needs its own A0,A1,A2 shape (CALLSTUB_A0A1A2, already defined above
 * for PackStructureTags/UnpackStructureTags -- same register shape,
 * different argument meanings); GetUniqueID needs the new zero-arg
 * CALLSTUB_NONE just above. */
CALLSTUB_A0A1(AddNamedObject, -222);
CALLSTUB_A0A1(AllocNamedObjectA, -228);
CALLSTUB_A0(AttemptRemNamedObject, -234);
CALLSTUB_A0A1A2(FindNamedObject, -240);
CALLSTUB_A0(FreeNamedObject, -246);
CALLSTUB_A0(NamedObjectName, -252);
CALLSTUB_A0(ReleaseNamedObject, -258);
CALLSTUB_A0A1(RemNamedObject, -264);
CALLSTUB_NONE(GetUniqueID, -270);

/* ======================================================================
 * Test data. User tag values deliberately well clear of the TAG_DONE/
 * TAG_IGNORE/TAG_MORE/TAG_SKIP control range (0-3).
 * ====================================================================== */

#define TT_ALPHA   100
#define TT_BETA    101
#define TT_GAMMA   102
#define TT_DELTA   103
#define TT_HIDDEN  104   /* lives in a TAG_SKIP-hidden slot; must never
                           * be visible to NextTagItem/FindTagItem */
#define TT_EPSILON 105   /* deliberately absent everywhere, for the
                           * "not found" / default-value paths */

static struct TagItem contTags[] = {
    { TT_DELTA, 4004 },
    { TAG_DONE, 0 },
};

/* Exercises TAG_IGNORE, TAG_SKIP (hiding TT_HIDDEN), and TAG_MORE
 * (chaining into contTags[], which also ends this array per the
 * autodoc). Flattened visible sequence: ALPHA, BETA, GAMMA, DELTA. */
static struct TagItem mainTags[] = {
    { TT_ALPHA, 1001 },
    { TAG_IGNORE, 0 },
    { TT_BETA, 1002 },
    { TAG_SKIP, 1 },
    { TT_HIDDEN, 9999 },
    { TT_GAMMA, 1003 },
    { TAG_MORE, (ULONG)contTags },
};

#define FLATTENED_COUNT 4
static const Tag flat_tags[FLATTENED_COUNT]  = { TT_ALPHA, TT_BETA, TT_GAMMA, TT_DELTA };
static const ULONG flat_data[FLATTENED_COUNT] = { 1001, 1002, 1003, 4004 };

/* Plain Tag array (NOT a TagItem list) -- TagInArray's distinct input
 * type. Deliberately includes TT_GAMMA (also in mainTags, to confirm the
 * two search paths don't get conflated) but omits TT_DELTA/TT_HIDDEN. */
static Tag plainArr[] = { TT_ALPHA, TT_GAMMA, TAG_DONE };

/* ======================================================================
 * Helpers.
 * ====================================================================== */

static BOOL tagListMatchesFlat(struct TagItem *list)
{
    ULONG i;

    if (!list)
        return FALSE;

    for (i = 0; i < FLATTENED_COUNT; i++) {
        if (list[i].ti_Tag != flat_tags[i] || list[i].ti_Data != flat_data[i])
            return FALSE;
    }
    /* Terminator slot. */
    return (list[FLATTENED_COUNT].ti_Tag == TAG_DONE);
}

static BOOL tagListsIdentical(struct TagItem *a, struct TagItem *b, ULONG n)
{
    ULONG i;

    if (!a || !b)
        return (a == b);

    for (i = 0; i < n; i++) {
        if (a[i].ti_Tag != b[i].ti_Tag || a[i].ti_Data != b[i].ti_Data)
            return FALSE;
    }
    return TRUE;
}

/* ======================================================================
 * The test cases. Each is run once per library base; ok = the new
 * library's own result matches the hand-derived expected value AND
 * matches the real ROM library's result bit-for-bit.
 * ====================================================================== */

static void test_next_tag_item(struct Library *newBase, struct Library *realBase)
{
    struct TagItem *newList[FLATTENED_COUNT + 1];
    struct TagItem *realList[FLATTENED_COUNT + 1];
    struct TagItem *tstate;
    struct TagItem *tag;
    ULONG newCount = 0, realCount = 0;
    ULONG i;
    BOOL ok;

    tstate = mainTags;
    while (newCount < FLATTENED_COUNT + 1 &&
           (tag = (struct TagItem *)NextTagItem_Call(newBase, &tstate)) != NULL) {
        newList[newCount++] = tag;
    }

    tstate = mainTags;
    while (realCount < FLATTENED_COUNT + 1 &&
           (tag = (struct TagItem *)NextTagItem_Call(realBase, &tstate)) != NULL) {
        realList[realCount++] = tag;
    }

    ok = (newCount == FLATTENED_COUNT) && (realCount == FLATTENED_COUNT);
    for (i = 0; ok && i < FLATTENED_COUNT; i++) {
        if (newList[i]->ti_Tag != flat_tags[i] || newList[i]->ti_Data != flat_data[i])
            ok = FALSE;
        if (realList[i]->ti_Tag != flat_tags[i] || realList[i]->ti_Data != flat_data[i])
            ok = FALSE;
    }

    result((CONST_STRPTR) "NextTagItem-SkipIgnoreMoreDone", ok);
}

static void test_find_tag_item(struct Library *newBase, struct Library *realBase)
{
    struct TagItem *newFound, *realFound;
    BOOL ok;

    newFound  = (struct TagItem *)FindTagItem_Call(newBase, TT_GAMMA, mainTags);
    realFound = (struct TagItem *)FindTagItem_Call(realBase, TT_GAMMA, mainTags);
    ok = newFound && realFound &&
         newFound->ti_Data == 1003 && realFound->ti_Data == 1003;
    result((CONST_STRPTR) "FindTagItem-Found", ok);

    newFound  = (struct TagItem *)FindTagItem_Call(newBase, TT_HIDDEN, mainTags);
    realFound = (struct TagItem *)FindTagItem_Call(realBase, TT_HIDDEN, mainTags);
    ok = (newFound == NULL) && (realFound == NULL);
    result((CONST_STRPTR) "FindTagItem-HiddenNotFound", ok);
}

static void test_get_tag_data(struct Library *newBase, struct Library *realBase)
{
    ULONG newVal, realVal;
    BOOL ok;

    newVal  = GetTagData_Call(newBase, TT_DELTA, 0, mainTags);
    realVal = GetTagData_Call(realBase, TT_DELTA, 0, mainTags);
    ok = (newVal == 4004) && (realVal == 4004);
    result((CONST_STRPTR) "GetTagData-Found", ok);

    newVal  = GetTagData_Call(newBase, TT_EPSILON, 777, mainTags);
    realVal = GetTagData_Call(realBase, TT_EPSILON, 777, mainTags);
    ok = (newVal == 777) && (realVal == 777);
    result((CONST_STRPTR) "GetTagData-DefaultNotFound", ok);
}

static void test_tag_in_array(struct Library *newBase, struct Library *realBase)
{
    ULONG newVal, realVal;
    BOOL ok;

    newVal  = TagInArray_Call(newBase, TT_GAMMA, plainArr);
    realVal = TagInArray_Call(realBase, TT_GAMMA, plainArr);
    ok = newVal && realVal;
    result((CONST_STRPTR) "TagInArray-Found", ok);

    /* TT_DELTA is a real tag in mainTags (a TagItem list) but NOT in
     * plainArr (a plain Tag array) -- the point of this sub-case. */
    newVal  = TagInArray_Call(newBase, TT_DELTA, plainArr);
    realVal = TagInArray_Call(realBase, TT_DELTA, plainArr);
    ok = (!newVal) && (!realVal);
    result((CONST_STRPTR) "TagInArray-NotFound", ok);
}

static void test_allocate_free_round_trip(struct Library *newBase, struct Library *realBase)
{
    struct TagItem *newAlloc, *realAlloc;
    BOOL ok;

    newAlloc  = (struct TagItem *)AllocateTagItems_Call(newBase, 4);
    realAlloc = (struct TagItem *)AllocateTagItems_Call(realBase, 4);

    ok = (newAlloc != NULL) && (realAlloc != NULL);
    if (ok) {
        /* MEMF_CLEAR'd per autodoc -- every slot should read back as
         * TAG_DONE/0 until filled. */
        ok = (newAlloc[0].ti_Tag == TAG_DONE) && (newAlloc[3].ti_Tag == TAG_DONE) &&
             (realAlloc[0].ti_Tag == TAG_DONE) && (realAlloc[3].ti_Tag == TAG_DONE);
    }
    if (ok) {
        newAlloc[0].ti_Tag = TT_ALPHA;  newAlloc[0].ti_Data = 111;
        newAlloc[1].ti_Tag = TT_BETA;   newAlloc[1].ti_Data = 222;
        realAlloc[0].ti_Tag = TT_ALPHA; realAlloc[0].ti_Data = 111;
        realAlloc[1].ti_Tag = TT_BETA;  realAlloc[1].ti_Data = 222;

        ok = tagListsIdentical(newAlloc, realAlloc, 4);
    }

    if (newAlloc)
        FreeTagItems_Call(newBase, newAlloc);
    if (realAlloc)
        FreeTagItems_Call(realBase, realAlloc);

    result((CONST_STRPTR) "AllocateTagItems-FreeTagItems-RoundTrip", ok);
}

static struct TagItem *newClone = NULL, *realClone = NULL;

static void test_clone_tag_items(struct Library *newBase, struct Library *realBase)
{
    BOOL ok;

    newClone  = (struct TagItem *)CloneTagItems_Call(newBase, mainTags);
    realClone = (struct TagItem *)CloneTagItems_Call(realBase, mainTags);

    ok = tagListMatchesFlat(newClone) && tagListMatchesFlat(realClone) &&
         tagListsIdentical(newClone, realClone, FLATTENED_COUNT + 1);

    result((CONST_STRPTR) "CloneTagItems-FlattenSkipMore", ok);
}

static void test_clone_tag_items_null(struct Library *newBase, struct Library *realBase)
{
    struct TagItem *newEmpty, *realEmpty;
    BOOL ok;

    newEmpty  = (struct TagItem *)CloneTagItems_Call(newBase, NULL);
    realEmpty = (struct TagItem *)CloneTagItems_Call(realBase, NULL);

    ok = newEmpty && realEmpty &&
         newEmpty[0].ti_Tag == TAG_DONE && newEmpty[0].ti_Data == 0 &&
         realEmpty[0].ti_Tag == TAG_DONE && realEmpty[0].ti_Data == 0;

    result((CONST_STRPTR) "CloneTagItems-NullEmpty", ok);

    if (newEmpty)
        FreeTagItems_Call(newBase, newEmpty);
    if (realEmpty)
        FreeTagItems_Call(realBase, realEmpty);
}

static void test_refresh_tag_item_clones(struct Library *newBase, struct Library *realBase)
{
    BOOL ok;

    if (!newClone || !realClone) {
        result((CONST_STRPTR) "RefreshTagItemClones-ResetAfterMutate", FALSE);
        return;
    }

    /* Mutate, then confirm the refresh puts it back. */
    newClone[0].ti_Data  = 0xDEAD;
    realClone[0].ti_Data = 0xDEAD;

    RefreshTagItemClones_Call(newBase, newClone, mainTags);
    RefreshTagItemClones_Call(realBase, realClone, mainTags);

    ok = tagListMatchesFlat(newClone) && tagListMatchesFlat(realClone) &&
         tagListsIdentical(newClone, realClone, FLATTENED_COUNT + 1);

    result((CONST_STRPTR) "RefreshTagItemClones-ResetAfterMutate", ok);

    FreeTagItems_Call(newBase, newClone);
    FreeTagItems_Call(realBase, realClone);
    newClone = realClone = NULL;
}

/* ======================================================================
 * PackBoolTags / FilterTagChanges / MapTags test data and cases.
 * Tag values deliberately well clear of the TAG_DONE/TAG_IGNORE/
 * TAG_MORE/TAG_SKIP control range (0-3) and of the TT_* values above.
 * ====================================================================== */

#define BT_ONE      110
#define BT_TWO      111
#define BT_THREE    112
#define BT_UNMAPPED 113

#define FT_SIZE     120
#define FT_COLOR    121
#define FT_SHAPE    122
#define FT_UNKNOWN  123

#define MT_ALPHA        130
#define MT_BETA         131
#define MT_GAMMA        132
#define MT_ALPHA_MAPPED 140

#define MAP_REMOVE_NOT_FOUND 0
#define MAP_KEEP_NOT_FOUND   1

static void test_pack_bool_tags(struct Library *newBase, struct Library *realBase)
{
    struct TagItem boolMap[] = {
        { BT_ONE,   0x0001 },
        { BT_TWO,   0x0002 },
        { BT_THREE, 0x0004 },
        { TAG_DONE, 0 },
    };
    /* BT_ONE appears twice (TRUE then FALSE) -- exercises the autodoc's
     * "last of duplicate tags will hold sway" WARNING. BT_UNMAPPED is
     * not in boolMap, so it must not affect the result at all. */
    struct TagItem boolExample[] = {
        { BT_ONE,      TRUE },
        { BT_TWO,      FALSE },
        { BT_UNMAPPED, 999 },
        { BT_ONE,      FALSE },
        { TAG_DONE,    0 },
    };
    ULONG initialFlags = 0x0008;
    ULONG newVal, realVal;
    BOOL ok;

    newVal  = (ULONG)PackBoolTags_Call(newBase,  initialFlags, boolExample, boolMap);
    realVal = (ULONG)PackBoolTags_Call(realBase, initialFlags, boolExample, boolMap);

    /* Hand-derived: start 0x0008; BT_ONE TRUE sets 0x0001 -> 0x0009;
     * BT_TWO FALSE clears 0x0002 (was already clear) -> 0x0009;
     * BT_UNMAPPED is ignored -> 0x0009; BT_ONE FALSE (the later
     * duplicate) clears 0x0001 -> 0x0008 final. */
    ok = (newVal == 0x0008) && (realVal == 0x0008);
    result((CONST_STRPTR) "PackBoolTags-DuplicateLastWinsUnmappedIgnored", ok);
}

static void test_filter_tag_changes(struct Library *newBase, struct Library *realBase, BOOL apply)
{
    struct TagItem newOrig[] = {
        { FT_SIZE,  100 },
        { FT_COLOR, 200 },
        { FT_SHAPE, 300 },
        { TAG_DONE, 0 },
    };
    struct TagItem newChange[] = {
        { FT_SIZE,    100 }, /* identical -- becomes TAG_IGNORE */
        { FT_SHAPE,   999 }, /* different -- survives; applied if apply */
        { FT_UNKNOWN, 55 },  /* absent from originalList -- untouched */
        { TAG_DONE,   0 },
    };
    struct TagItem realOrig[] = {
        { FT_SIZE,  100 },
        { FT_COLOR, 200 },
        { FT_SHAPE, 300 },
        { TAG_DONE, 0 },
    };
    struct TagItem realChange[] = {
        { FT_SIZE,    100 },
        { FT_SHAPE,   999 },
        { FT_UNKNOWN, 55 },
        { TAG_DONE,   0 },
    };
    BOOL ok;

    FilterTagChanges_Call(newBase,  newChange,  newOrig,  (ULONG)apply);
    FilterTagChanges_Call(realBase, realChange, realOrig, (ULONG)apply);

    ok = tagListsIdentical(newChange, realChange, 4) &&
         newChange[0].ti_Tag == TAG_IGNORE && newChange[0].ti_Data == 100 &&
         newChange[1].ti_Tag == FT_SHAPE && newChange[1].ti_Data == 999 &&
         newChange[2].ti_Tag == FT_UNKNOWN && newChange[2].ti_Data == 55;

    if (ok) {
        if (apply) {
            /* originalList's FT_SHAPE must have been updated to 999. */
            ok = tagListsIdentical(newOrig, realOrig, 3) &&
                 newOrig[0].ti_Data == 100 && newOrig[1].ti_Data == 200 &&
                 newOrig[2].ti_Data == 999;
        } else {
            /* originalList must be completely unchanged. */
            ok = tagListsIdentical(newOrig, realOrig, 3) &&
                 newOrig[0].ti_Data == 100 && newOrig[1].ti_Data == 200 &&
                 newOrig[2].ti_Data == 300;
        }
    }

    result(apply ? (CONST_STRPTR) "FilterTagChanges-ApplyTrue"
                 : (CONST_STRPTR) "FilterTagChanges-ApplyFalse", ok);
}

/* MapTags-RemoveNotFound/KeepNotFound: this is the empirical test for
 * the autodoc's own documented pre-V39 "mapType ignored, always behaves
 * as MAP_KEEP_NOT_FOUND" BUG. This project's baseline is V40 (post-V39),
 * so the fix is expected -- but the project's "the oracle wins over the
 * docs" rule means this is settled by comparing against the real
 * ROM-resident utility.library directly, not by trusting the version
 * gate. MT_GAMMA (absent from mapList) is the discriminator: under
 * MAP_REMOVE_NOT_FOUND it must become TAG_IGNORE; under
 * MAP_KEEP_NOT_FOUND it must survive unchanged as MT_GAMMA. MT_BETA maps
 * to TAG_DONE in mapList, exercising the "mapped-to-TAG_DONE becomes
 * TAG_IGNORE instead" safety substitution. */
static void test_map_tags(struct Library *newBase, struct Library *realBase, ULONG mapType)
{
    struct TagItem mapList[] = {
        { MT_ALPHA, MT_ALPHA_MAPPED },
        { MT_BETA,  TAG_DONE },
        { TAG_DONE, 0 },
    };
    struct TagItem newList[] = {
        { MT_ALPHA, 10 },
        { MT_BETA,  20 },
        { MT_GAMMA, 30 },
        { TAG_DONE, 0 },
    };
    struct TagItem realList[] = {
        { MT_ALPHA, 10 },
        { MT_BETA,  20 },
        { MT_GAMMA, 30 },
        { TAG_DONE, 0 },
    };
    BOOL ok;

    MapTags_Call(newBase,  newList,  mapList, mapType);
    MapTags_Call(realBase, realList, mapList, mapType);

    ok = tagListsIdentical(newList, realList, 4) &&
         newList[0].ti_Tag == MT_ALPHA_MAPPED && newList[0].ti_Data == 10 &&
         newList[1].ti_Tag == TAG_IGNORE && newList[1].ti_Data == 20;

    if (ok) {
        if (mapType == MAP_REMOVE_NOT_FOUND) {
            ok = (newList[2].ti_Tag == TAG_IGNORE) && (realList[2].ti_Tag == TAG_IGNORE);
        } else {
            ok = (newList[2].ti_Tag == MT_GAMMA) && (realList[2].ti_Tag == MT_GAMMA) &&
                 (newList[2].ti_Data == 30) && (realList[2].ti_Data == 30);
        }
    }

    result(mapType == MAP_REMOVE_NOT_FOUND ? (CONST_STRPTR) "MapTags-RemoveNotFound"
                                            : (CONST_STRPTR) "MapTags-KeepNotFound", ok);
}

/* ======================================================================
 * FilterTagItems test data and cases. Tag values deliberately well
 * clear of the TAG_DONE/TAG_IGNORE/TAG_MORE/TAG_SKIP control range and
 * of every other TT_, BT_, FT_, MT_ value above.
 * ====================================================================== */

#define FI_SIZE    150
#define FI_COLOR   151
#define FI_SHAPE   152
#define FI_UNKNOWN 153

static void test_filter_tag_items(struct Library *newBase, struct Library *realBase, ULONG logic)
{
    Tag filterArray[] = { FI_SIZE, FI_SHAPE, TAG_DONE };
    struct TagItem newList[] = {
        { FI_SIZE,    1 },
        { FI_COLOR,   2 },
        { FI_SHAPE,   3 },
        { FI_UNKNOWN, 4 },
        { TAG_DONE,   0 },
    };
    struct TagItem realList[] = {
        { FI_SIZE,    1 },
        { FI_COLOR,   2 },
        { FI_SHAPE,   3 },
        { FI_UNKNOWN, 4 },
        { TAG_DONE,   0 },
    };
    ULONG newValid, realValid;
    BOOL ok;

    /* LVO -96 is A0=tagList, A1=filterArray, D0=logic -- CALLSTUB_A0A1D0's
     * positional args map straight through as (base, a0v, a1v, d0v), so
     * tagList (the A0 argument) must come first here, filterArray (A1)
     * second. (An earlier draft of this call had these two swapped --
     * caught by the real rig's own FAIL, not by inspection: see
     * provenance/utility-library-datefilter.md's "Errors and fixes".) */
    newValid  = FilterTagItems_Call(newBase,  newList,  filterArray, logic);
    realValid = FilterTagItems_Call(realBase, realList, filterArray, logic);

    ok = tagListsIdentical(newList, realList, 5) && (newValid == realValid);

    if (ok) {
        if (logic == TAGFILTER_AND) {
            /* Everything NOT in filterArray (FI_COLOR, FI_UNKNOWN) is
             * excluded; FI_SIZE/FI_SHAPE survive. numValid == 2. */
            ok = (newValid == 2) &&
                 newList[0].ti_Tag == FI_SIZE && newList[1].ti_Tag == TAG_IGNORE &&
                 newList[2].ti_Tag == FI_SHAPE && newList[3].ti_Tag == TAG_IGNORE;
        } else {
            /* TAGFILTER_NOT: items IN filterArray (FI_SIZE, FI_SHAPE)
             * are cast out; FI_COLOR/FI_UNKNOWN survive. numValid == 2. */
            ok = (newValid == 2) &&
                 newList[0].ti_Tag == TAG_IGNORE && newList[1].ti_Tag == FI_COLOR &&
                 newList[2].ti_Tag == TAG_IGNORE && newList[3].ti_Tag == FI_UNKNOWN;
        }
    }

    result(logic == TAGFILTER_AND ? (CONST_STRPTR) "FilterTagItems-TagfilterAnd"
                                   : (CONST_STRPTR) "FilterTagItems-TagfilterNot", ok);
}

/* ======================================================================
 * CallHookPkt test data and case (LVO -102, fifth work package --
 * provenance/utility-library-callhook.md).
 *
 * A stub Hook whose h_Entry does nothing useful would prove nothing
 * about the dispatch mechanism (CallHookPkt_Entry's own job is entirely
 * "fetch hook->h_Entry, JSR through it" -- see entry.c's own comment
 * at its call site). This builds a REAL struct Hook with a genuine,
 * hand-written asm h_Entry (TestHookEntry below) that actually receives
 * A0=hook/A1=message/A2=object per <utility/hooks.h>'s own documented
 * convention and computes a value derived from both object and message
 * -- object points to a ULONG "base" value, message points to a ULONG
 * "addend"; the hook returns base+addend in D0. Calling this through
 * BOTH library bases with the SAME hook and checking the SAME computed
 * result against both is the actual proof that this project's
 * CallHookPkt dispatches correctly end to end (register plumbing all
 * the way through to a real callee and back), not just that it
 * compiles or returns some value.
 *
 * TestHookEntry is NOT a stack-ABI C function wrapped the way <utility/
 * hooks.h>'s own "vanilla C parameter conventions" example stub is
 * (that pattern exists for callers who want h_SubEntry to be an
 * ordinary stack-ABI C function) -- this test has no need for a second
 * layer, so h_Entry itself is the hand-written asm, directly computing
 * the result from A1/A2 and returning it in D0, matching exactly what
 * CallHookPkt_Entry's own "it's really very simple" framing promises:
 * whatever h_Entry leaves in D0 is CallHookPkt's own return value, with
 * nothing in between.
 * ====================================================================== */

static ULONG hookObjectVal;
static ULONG hookMessageVal;

extern APTR TestHookEntry(void);
asm(".text");
asm(".globl _TestHookEntry");
asm("_TestHookEntry:");
asm("\tmove.l (a2),d0");   /* d0 = *object (the "base" value) */
asm("\tadd.l (a1),d0");    /* d0 += *message (the "addend")   */
asm("\trts");

static struct Hook testHook;

static void test_call_hook_pkt(struct Library *newBase, struct Library *realBase)
{
    ULONG newResult, realResult;
    BOOL ok;
    struct HookProbeResult newProbe, realProbe;
    BOOL a3ok;

    testHook.h_MinNode.mln_Succ = NULL;
    testHook.h_MinNode.mln_Pred = NULL;
    testHook.h_Entry = (ULONG (*)())TestHookEntry;
    testHook.h_SubEntry = NULL;  /* unused -- TestHookEntry is the whole hook */
    testHook.h_Data = NULL;      /* unused -- object/message are passed explicitly */

    hookObjectVal = 1000;
    hookMessageVal = 234;

    newResult  = CallHookPkt_Call(newBase,  &testHook, &hookObjectVal, &hookMessageVal);
    realResult = CallHookPkt_Call(realBase, &testHook, &hookObjectVal, &hookMessageVal);

    ok = (newResult == 1234UL) && (realResult == 1234UL);

    result((CONST_STRPTR) "CallHookPkt-DispatchAndCompute", ok);

    /* A3-preservation oracle (2026-10-13 correction, provenance/
     * utility-library-callhook.md) -- loads a recognizable sentinel into
     * A3 immediately before the call (CALLSTUB_A0A2A1_A3PROBE above) and
     * confirms it's still there immediately after, against BOTH library
     * bases. This is the empirical proof that this project's own
     * CallHookPkt (post-fix: a bare stack-swap tail jump that touches no
     * register at all) preserves A3 exactly like the real ROM does --
     * the same technique the math family already uses for its own
     * documented A0/A1-preservation guarantee (MATH_SENTINEL_A0/A1), just
     * applied here to the ordinary RKRM "every register except
     * D0/D1/A0/A1 is preserved" rule rather than a function-specific
     * documented exception. */
    CallHookPkt_CallA3Probe(newBase,  &testHook, &hookObjectVal, &hookMessageVal, &newProbe);
    CallHookPkt_CallA3Probe(realBase, &testHook, &hookObjectVal, &hookMessageVal, &realProbe);

    a3ok = (newProbe.d0 == 1234UL)  && (newProbe.a3  == HOOK_SENTINEL_A3) &&
           (realProbe.d0 == 1234UL) && (realProbe.a3 == HOOK_SENTINEL_A3);

    result((CONST_STRPTR) "CallHookPkt-A3Preserved", a3ok);
}

/* ======================================================================
 * Amiga2Date / Date2Amiga / CheckDate test cases.
 *
 * Expected seconds/wday values below are hand-derived via a Python
 * `datetime.date` reference computation (NOT eyeballed -- see this
 * work package's own task-spec warning about exactly that mistake in
 * an earlier package's math tests), using 01-Jan-1978 (itself a
 * Sunday, wday 0) as the epoch:
 *
 *   1978-01-01 00:00:00           -> seconds=0,         wday=0 (Sun)
 *   2000-02-29 12:30:15 (leap)    -> seconds=699366615,  wday=2 (Tue)
 *   1979-02-28 23:59:59 (nonleap) -> seconds=36633599,   wday=3 (Wed)
 *   1979-03-01 00:00:00 (nonleap) -> seconds=36633600,   wday=4 (Thu)
 * ====================================================================== */

static BOOL clockDataEqual(struct ClockData *a, struct ClockData *b)
{
    return a->sec == b->sec && a->min == b->min && a->hour == b->hour &&
           a->mday == b->mday && a->month == b->month && a->year == b->year &&
           a->wday == b->wday;
}

static void test_amiga2date_epoch(struct Library *newBase, struct Library *realBase)
{
    struct ClockData newCD, realCD, expected;
    BOOL ok;

    Amiga2Date_Call(newBase, 0, &newCD);
    Amiga2Date_Call(realBase, 0, &realCD);

    expected.sec = 0; expected.min = 0; expected.hour = 0;
    expected.mday = 1; expected.month = 1; expected.year = 1978; expected.wday = 0;

    ok = clockDataEqual(&newCD, &expected) && clockDataEqual(&realCD, &expected);

    result((CONST_STRPTR) "Amiga2Date-EpochBoundary", ok);
}

static void test_date_leap_feb29(struct Library *newBase, struct Library *realBase)
{
    struct ClockData date, newBack, realBack;
    ULONG newSecs, realSecs;
    BOOL ok;

    date.sec = 15; date.min = 30; date.hour = 12;
    date.mday = 29; date.month = 2; date.year = 2000;
    date.wday = 2; /* correct, for the round-trip comparison below */

    newSecs  = Date2Amiga_Call(newBase,  &date);
    realSecs = Date2Amiga_Call(realBase, &date);

    ok = (newSecs == 699366615UL) && (realSecs == 699366615UL);

    if (ok) {
        Amiga2Date_Call(newBase, newSecs, &newBack);
        Amiga2Date_Call(realBase, realSecs, &realBack);
        ok = clockDataEqual(&newBack, &date) && clockDataEqual(&realBack, &date);
    }

    result((CONST_STRPTR) "Date2Amiga-LeapYearFeb29RoundTrip", ok);
}

static void test_date_nonleap_boundary(struct Library *newBase, struct Library *realBase)
{
    struct ClockData before, after;
    ULONG newBefore, realBefore, newAfter, realAfter;
    BOOL ok;

    /* 1979 is not a leap year: Feb has 28 days, so 23:59:59 on the 28th
     * must be exactly one second before 00:00:00 on 1-Mar. */
    before.sec = 59; before.min = 59; before.hour = 23;
    before.mday = 28; before.month = 2; before.year = 1979; before.wday = 3;

    after.sec = 0; after.min = 0; after.hour = 0;
    after.mday = 1; after.month = 3; after.year = 1979; after.wday = 4;

    newBefore  = Date2Amiga_Call(newBase,  &before);
    realBefore = Date2Amiga_Call(realBase, &before);
    newAfter   = Date2Amiga_Call(newBase,  &after);
    realAfter  = Date2Amiga_Call(realBase, &after);

    ok = (newBefore == 36633599UL) && (realBefore == 36633599UL) &&
         (newAfter  == 36633600UL) && (realAfter  == 36633600UL) &&
         (newAfter == newBefore + 1) && (realAfter == realBefore + 1);

    result((CONST_STRPTR) "Date2Amiga-NonLeapFebMarBoundary", ok);
}

/* CheckDate-WdayNotChecked: the empirical question this work package's
 * task spec flagged -- does the real V40 oracle genuinely skip
 * validating wday, per the autodoc's own documented BUG, or was that
 * fixed by this project's baseline version? A deliberately out-of-range
 * wday (99, autodoc's valid range is 0-6) on an otherwise-100%-valid
 * date must still be accepted (non-zero/matching result) if the bug is
 * still present. */
static void test_checkdate_wday_not_checked(struct Library *newBase, struct Library *realBase)
{
    struct ClockData date;
    ULONG newResult, realResult;
    BOOL ok;

    date.sec = 15; date.min = 30; date.hour = 12;
    date.mday = 29; date.month = 2; date.year = 2000;
    date.wday = 99; /* deliberately out of the documented 0-6 range */

    newResult  = CheckDate_Call(newBase,  &date);
    realResult = CheckDate_Call(realBase, &date);

    ok = (newResult == 699366615UL) && (realResult == 699366615UL);

    result((CONST_STRPTR) "CheckDate-WdayNotChecked", ok);
}

static void test_checkdate_invalid_mday(struct Library *newBase, struct Library *realBase)
{
    struct ClockData date;
    ULONG newResult, realResult;
    BOOL ok;

    /* 1979 is not a leap year -- Feb only has 28 days, so mday=29 is
     * illegal. wday is left correct here, to isolate the mday check
     * from the wday-not-checked question above. */
    date.sec = 0; date.min = 0; date.hour = 0;
    date.mday = 29; date.month = 2; date.year = 1979; date.wday = 4;

    newResult  = CheckDate_Call(newBase,  &date);
    realResult = CheckDate_Call(realBase, &date);

    ok = (newResult == 0) && (realResult == 0);

    result((CONST_STRPTR) "CheckDate-InvalidMdayRejected", ok);
}

/* ======================================================================
 * SMult32 / UMult32 / SDivMod32 / UDivMod32 test cases.
 *
 * Each case does TWO things in one call: checks the arithmetic result
 * against a hand-derived expected value (the "easy, obvious part" per
 * this work package's own task spec) AND checks that A0/A1 survived the
 * call unchanged from the sentinels loaded immediately before the JSR
 * (the actual point of this work package's Step 1 -- confirmed against
 * the real ROM, not assumed).
 * ====================================================================== */

static void test_smult32(struct Library *newBase, struct Library *realBase)
{
    struct MathResult newR, realR;
    BOOL ok;

    /* -7 * 6 = -42, well clear of overflow. */
    SMult32_Call(newBase, (ULONG)(LONG)-7, (ULONG)(LONG)6, &newR);
    SMult32_Call(realBase, (ULONG)(LONG)-7, (ULONG)(LONG)6, &realR);

    ok = ((LONG)newR.d0 == -42) && ((LONG)realR.d0 == -42) &&
         newR.a0 == MATH_SENTINEL_A0 && newR.a1 == MATH_SENTINEL_A1 &&
         realR.a0 == MATH_SENTINEL_A0 && realR.a1 == MATH_SENTINEL_A1;

    result((CONST_STRPTR) "SMult32-ResultAndA0A1Preserved", ok);
}

static void test_umult32(struct Library *newBase, struct Library *realBase)
{
    struct MathResult newR, realR;
    BOOL ok;

    /* 50000 * 50000 = 2,500,000,000 -- deliberately overflows a signed
     * 32-bit LONG (> 2^31-1 = 2,147,483,647) but fits an unsigned 32-bit
     * ULONG (< 2^32-1 = 4,294,967,295), exercising the signed/unsigned
     * distinction between this function and SMult32. (An earlier draft
     * of this test used 70000*70000 = 4,900,000,000 as the "deliberate
     * overflow" case without checking that it also overflows ULONG's
     * own 32-bit range -- caught by hand-verifying the expected value in
     * Python before trusting it, a reminder to actually compute these
     * rather than eyeball them.) */
    UMult32_Call(newBase, 50000UL, 50000UL, &newR);
    UMult32_Call(realBase, 50000UL, 50000UL, &realR);

    ok = (newR.d0 == 2500000000UL) && (realR.d0 == 2500000000UL) &&
         newR.a0 == MATH_SENTINEL_A0 && newR.a1 == MATH_SENTINEL_A1 &&
         realR.a0 == MATH_SENTINEL_A0 && realR.a1 == MATH_SENTINEL_A1;

    result((CONST_STRPTR) "UMult32-ResultAndA0A1Preserved", ok);
}

static void test_sdivmod32(struct Library *newBase, struct Library *realBase)
{
    struct MathResult newR, realR;
    BOOL ok;

    /* -100 / 7 = -14 remainder -2 (C truncating-toward-zero division). */
    SDivMod32_Call(newBase, (ULONG)(LONG)-100, (ULONG)(LONG)7, &newR);
    SDivMod32_Call(realBase, (ULONG)(LONG)-100, (ULONG)(LONG)7, &realR);

    ok = ((LONG)newR.d0 == -14) && ((LONG)newR.d1 == -2) &&
         ((LONG)realR.d0 == -14) && ((LONG)realR.d1 == -2) &&
         newR.a0 == MATH_SENTINEL_A0 && newR.a1 == MATH_SENTINEL_A1 &&
         realR.a0 == MATH_SENTINEL_A0 && realR.a1 == MATH_SENTINEL_A1;

    result((CONST_STRPTR) "SDivMod32-ResultAndA0A1Preserved", ok);
}

static void test_udivmod32(struct Library *newBase, struct Library *realBase)
{
    struct MathResult newR, realR;
    BOOL ok;

    /* 100 / 7 = 14 remainder 2. */
    UDivMod32_Call(newBase, 100UL, 7UL, &newR);
    UDivMod32_Call(realBase, 100UL, 7UL, &realR);

    ok = (newR.d0 == 14UL) && (newR.d1 == 2UL) &&
         (realR.d0 == 14UL) && (realR.d1 == 2UL) &&
         newR.a0 == MATH_SENTINEL_A0 && newR.a1 == MATH_SENTINEL_A1 &&
         realR.a0 == MATH_SENTINEL_A0 && realR.a1 == MATH_SENTINEL_A1;

    result((CONST_STRPTR) "UDivMod32-ResultAndA0A1Preserved", ok);
}

/* ======================================================================
 * SMult64 / UMult64 test cases (seventh work package, provenance/
 * utility-library-math64.md). Deliberately chosen to overflow a plain
 * 32-bit truncated product -- the whole point of these test cases is
 * to distinguish "genuine 64-bit product" from "same truncated value
 * SMult32/UMult32 would give", which a value fitting in 32 bits could
 * never do (same lesson the UMult32 test-authoring mistake already
 * flagged: compute the expected value, don't eyeball it).
 *
 * 70000*70000 = 4,900,000,000 (hand-verified: 70000^2 = 7^2 * 10^8 =
 * 49 * 10^8 = 4,900,000,000) -- this is exactly the value an earlier
 * draft of the UMult32 test mistakenly tried to use (it overflows
 * ULONG's own 32-bit range, 4,294,967,295, which is precisely why it
 * was wrong for UMult32 but is exactly the right kind of case here).
 * As a 64-bit value: hi = 1, lo = 4,900,000,000 - 2^32 = 605,032,704
 * (0x24101100).
 * ====================================================================== */

static void test_umult64(struct Library *newBase, struct Library *realBase)
{
    struct MathResult newR, realR;
    BOOL ok;

    UMult64_Call(newBase, 70000UL, 70000UL, &newR);
    UMult64_Call(realBase, 70000UL, 70000UL, &realR);

    ok = (newR.d0 == 1UL) && (newR.d1 == 605032704UL) &&
         (realR.d0 == 1UL) && (realR.d1 == 605032704UL) &&
         newR.a0 == MATH_SENTINEL_A0 && newR.a1 == MATH_SENTINEL_A1 &&
         realR.a0 == MATH_SENTINEL_A0 && realR.a1 == MATH_SENTINEL_A1;

    result((CONST_STRPTR) "UMult64-ResultAndA0A1Preserved", ok);
}

/* SMult64's negative-result path has a confirmed, reproducible
 * divergence from the autodoc's plain "signed 64 bit result" framing
 * on the real V40 ROM -- see src/math.c's own
 * "DOC-VS-ORACLE DIVERGENCE" comment above SMult64 for the full
 * four-data-point derivation (gathered via exactly these calls, plus
 * one now-removed diagnostic case, during this work package's own
 * authoring). These three cases exercise all three branches the
 * derived formula distinguishes: a negative result whose magnitude
 * product overflows 32 bits (low word affected), a negative result
 * whose magnitude product fits in 32 bits (high word affected
 * instead), and a positive result from two negative operands
 * (unaffected, the magnitude product is the literal answer). Expected
 * values below are the REAL ORACLE's values, not the mathematically
 * "clean" ones -- this project replaces real V40 behaviour, bugs
 * included, per the project's "the oracle wins over the docs" rule. */

static void test_smult64_large_negative(struct Library *newBase, struct Library *realBase)
{
    struct MathResult newR, realR;
    BOOL ok;

    /* -70000 * 70000: mathematically -4,900,000,000, but the real ROM's
     * own negation quirk gives D0:D1 = 0xFFFFFFFF:0xDBEFEEFF (one more
     * than correct in the high word, one less than correct in the low
     * word) because the unsigned magnitude product's high word (1) is
     * nonzero. */
    SMult64_Call(newBase, (ULONG)(LONG)-70000, 70000UL, &newR);
    SMult64_Call(realBase, (ULONG)(LONG)-70000, 70000UL, &realR);

    ok = (newR.d0 == 0xFFFFFFFFUL) && (newR.d1 == 0xDBEFEEFFUL) &&
         (realR.d0 == 0xFFFFFFFFUL) && (realR.d1 == 0xDBEFEEFFUL) &&
         newR.a0 == MATH_SENTINEL_A0 && newR.a1 == MATH_SENTINEL_A1 &&
         realR.a0 == MATH_SENTINEL_A0 && realR.a1 == MATH_SENTINEL_A1;

    result((CONST_STRPTR) "SMult64-LargeNegativeOracleQuirk", ok);
}

static void test_smult64_small_negative(struct Library *newBase, struct Library *realBase)
{
    struct MathResult newR, realR;
    BOOL ok;

    /* -7 * 6: mathematically -42, but the real ROM's own negation quirk
     * gives D0:D1 = 0x00000000:0xFFFFFFD6 (the high word doesn't get
     * the correct all-1s sign extension -- it wraps to 0 -- while the
     * low word, 0xFFFFFFD6, happens to be correct here) because the
     * unsigned magnitude product's high word (0) IS zero -- the other
     * branch of the same quirk. */
    SMult64_Call(newBase, (ULONG)(LONG)-7, 6UL, &newR);
    SMult64_Call(realBase, (ULONG)(LONG)-7, 6UL, &realR);

    ok = (newR.d0 == 0x00000000UL) && (newR.d1 == 0xFFFFFFD6UL) &&
         (realR.d0 == 0x00000000UL) && (realR.d1 == 0xFFFFFFD6UL) &&
         newR.a0 == MATH_SENTINEL_A0 && newR.a1 == MATH_SENTINEL_A1 &&
         realR.a0 == MATH_SENTINEL_A0 && realR.a1 == MATH_SENTINEL_A1;

    result((CONST_STRPTR) "SMult64-SmallNegativeOracleQuirk", ok);
}

static void test_smult64_both_negative(struct Library *newBase, struct Library *realBase)
{
    struct MathResult newR, realR;
    BOOL ok;

    /* -70000 * -70000 = +4,900,000,000 -- a POSITIVE result from two
     * negative operands. Confirmed unaffected by the negation quirk
     * above (there's no negation to apply): D0:D1 = 0x00000001:
     * 0x24101100, the mathematically exact magnitude product. */
    SMult64_Call(newBase, (ULONG)(LONG)-70000, (ULONG)(LONG)-70000, &newR);
    SMult64_Call(realBase, (ULONG)(LONG)-70000, (ULONG)(LONG)-70000, &realR);

    ok = (newR.d0 == 1UL) && (newR.d1 == 605032704UL) &&
         (realR.d0 == 1UL) && (realR.d1 == 605032704UL) &&
         newR.a0 == MATH_SENTINEL_A0 && newR.a1 == MATH_SENTINEL_A1 &&
         realR.a0 == MATH_SENTINEL_A0 && realR.a1 == MATH_SENTINEL_A1;

    result((CONST_STRPTR) "SMult64-BothNegativeUnaffected", ok);
}

/* ======================================================================
 * ToUpper / ToLower / Stricmp / Strnicmp / ApplyTagChanges test cases
 * (sixth work package, LVO -162 through -186 -- provenance/
 * utility-library-stringops.md).
 * ====================================================================== */

/* Exhaustive, not sampled: every byte value 0x00-0xFF, both directions.
 * The "handling international character sets" autodoc text is vague on
 * its own; per the oracle-wins rule this project's own, from-scratch
 * implementation (src/) is checked byte-for-byte against the
 * real, un-patched (no locale.library) V40 ROM rather than assumed to
 * match any particular hypothesis. Any mismatch is reported individually
 * (DIAG-CaseMismatch) so a failing run's own serial transcript identifies
 * exactly which byte(s) diverged, not just that something did. */
static void test_toupper_tolower_full_range(struct Library *newBase, struct Library *realBase)
{
    ULONG i;
    BOOL ok = TRUE;

    for (i = 0; i < 256; i++) {
        ULONG newUp  = ToUpper_Call(newBase,  i);
        ULONG realUp = ToUpper_Call(realBase, i);
        ULONG newLo  = ToLower_Call(newBase,  i);
        ULONG realLo = ToLower_Call(realBase, i);

        if (newUp != realUp || newLo != realLo) {
            ULONG args[5];
            args[0] = i;
            args[1] = newUp;
            args[2] = realUp;
            args[3] = newLo;
            args[4] = realLo;
            ok = FALSE;
            report((CONST_STRPTR) "DIAG-CaseMismatch byte=%lu newUp=%lu realUp=%lu newLo=%lu realLo=%lu\r\n", args);
        }
    }

    result((CONST_STRPTR) "ToUpper-ToLower-FullByteRange", ok);
}

static void test_stricmp(struct Library *newBase, struct Library *realBase)
{
    LONG newVal, realVal;
    BOOL ok;

    /* Case-insensitive equal, equal length. */
    newVal  = Stricmp_Call(newBase,  (APTR)"Hello", (APTR)"HELLO");
    realVal = Stricmp_Call(realBase, (APTR)"Hello", (APTR)"HELLO");
    ok = (newVal == realVal) && (newVal == 0);
    result((CONST_STRPTR) "Stricmp-CaseInsensitiveEqual", ok);

    /* Different lengths -- the autodoc's "shorter treated as extended
     * with zeros" rule: "ABC" vs "AB" must NOT compare equal just
     * because "AB" is a common prefix -- the longer string's trailing
     * 'C' must compare greater than the implicit zero/NUL the shorter
     * string is extended with at that position. The real ROM's own
     * result value is normalized to exactly -1/0/1 (confirmed via this
     * work package's own golden-test recording, not assumed -- see
     * provenance/utility-library-stringops.md), not a raw byte
     * difference, so this checks the exact value, not just the sign. */
    newVal  = Stricmp_Call(newBase,  (APTR)"ABC", (APTR)"AB");
    realVal = Stricmp_Call(realBase, (APTR)"ABC", (APTR)"AB");
    ok = (newVal == realVal) && (newVal == 1);
    result((CONST_STRPTR) "Stricmp-DifferentLengthsLongerGreater", ok);

    /* Same rule, operands swapped and mixed case -- sign flips. */
    newVal  = Stricmp_Call(newBase,  (APTR)"ab", (APTR)"ABC");
    realVal = Stricmp_Call(realBase, (APTR)"ab", (APTR)"ABC");
    ok = (newVal == realVal) && (newVal == -1);
    result((CONST_STRPTR) "Stricmp-DifferentLengthsShorterLess", ok);
}

static void test_strnicmp(struct Library *newBase, struct Library *realBase)
{
    LONG newVal, realVal;
    BOOL ok;

    /* length (5) shorter than either string -- the two strings only
     * differ at/after position 5, so must compare equal within the
     * length window regardless of what follows. */
    newVal  = Strnicmp_Call(newBase,  (APTR)"HELLOX", (APTR)"helloy", 5);
    realVal = Strnicmp_Call(realBase, (APTR)"HELLOX", (APTR)"helloy", 5);
    ok = (newVal == realVal) && (newVal == 0);
    result((CONST_STRPTR) "Strnicmp-LengthShorterThanBothStrings", ok);

    /* length (5) longer than the shorter string ("AB", length 2) --
     * falls through to the same "shorter extended with zeros" rule
     * Stricmp exercises above, within the Strnicmp length bound. Exact
     * -1/0/1 normalization, same as Stricmp (see that function's own
     * test case comment above). */
    newVal  = Strnicmp_Call(newBase,  (APTR)"AB", (APTR)"ABC", 5);
    realVal = Strnicmp_Call(realBase, (APTR)"AB", (APTR)"ABC", 5);
    ok = (newVal == realVal) && (newVal == -1);
    result((CONST_STRPTR) "Strnicmp-LengthLongerThanShorterString", ok);
}

#define AT_ALPHA 160
#define AT_BETA  161
#define AT_GAMMA 162  /* only ever in changeList, never in list */

/* Covers all three empirical questions this work package's task spec
 * flagged: a tag present in both lists (list's ti_Data must update to
 * changeList's value), a tag only in changeList (must be ignored --
 * NOT added to list), and whether changeList itself survives completely
 * unmodified (distinct from FilterTagChanges(), which DOES mutate
 * changeList -- converting matched/identical entries' ti_Tag to
 * TAG_IGNORE; ApplyTagChanges()'s own autodoc text never describes any
 * such mutation). */
static void test_apply_tag_changes(struct Library *newBase, struct Library *realBase)
{
    struct TagItem newList[] = {
        { AT_ALPHA, 10 },
        { AT_BETA,  20 },
        { TAG_DONE, 0 },
    };
    struct TagItem realList[] = {
        { AT_ALPHA, 10 },
        { AT_BETA,  20 },
        { TAG_DONE, 0 },
    };
    struct TagItem newChange[] = {
        { AT_ALPHA, 999 },
        { AT_GAMMA, 777 },
        { TAG_DONE, 0 },
    };
    struct TagItem realChange[] = {
        { AT_ALPHA, 999 },
        { AT_GAMMA, 777 },
        { TAG_DONE, 0 },
    };
    struct TagItem newChangeBefore[3];
    ULONG i;
    BOOL ok;

    for (i = 0; i < 3; i++)
        newChangeBefore[i] = newChange[i];

    ApplyTagChanges_Call(newBase,  newList,  newChange);
    ApplyTagChanges_Call(realBase, realList, realChange);

    /* list: AT_ALPHA updated to 999, AT_BETA untouched, no AT_GAMMA
     * entry added (terminator still sits at index 2). */
    ok = tagListsIdentical(newList, realList, 3) &&
         newList[0].ti_Tag == AT_ALPHA && newList[0].ti_Data == 999 &&
         newList[1].ti_Tag == AT_BETA  && newList[1].ti_Data == 20  &&
         newList[2].ti_Tag == TAG_DONE;

    if (ok) {
        /* changeList itself must be byte-for-byte unchanged by the call
         * -- the actual empirical question this test case exists to
         * settle, not just the list-update side. */
        for (i = 0; i < 3; i++) {
            if (newChange[i].ti_Tag != newChangeBefore[i].ti_Tag ||
                newChange[i].ti_Data != newChangeBefore[i].ti_Data) {
                ok = FALSE;
                break;
            }
        }
    }

    if (ok)
        ok = tagListsIdentical(newChange, realChange, 3);

    result((CONST_STRPTR) "ApplyTagChanges-UpdateIgnoreUnmodifiedChangeList", ok);
}

/* ======================================================================
 * Eighth work package (provenance/utility-library-packtags.md):
 * PackStructureTags (-210), UnpackStructureTags (-216). One shared test
 * struct covering every type the pack-table bytecode can address
 * (signed/unsigned Byte/Word/Long, a flags byte for Bit-type entries, a
 * dedicated field reached only via a PACK_NEWOFFSET() base-tag switch,
 * and a dedicated field for the "tag never present" skip case), and one
 * shared pack table built with the real NDK <utility/pack.h>
 * PACK_*()/PKCTRL_*() macros -- not hand-rolled bit patterns.
 * ====================================================================== */

struct PackTestStruct {
    BYTE  sByte;
    UBYTE uByte;
    WORD  sWord;
    UWORD uWord;
    LONG  sLong;
    ULONG uLong;
    UBYTE flags;
    UBYTE chainByte;
    UBYTE missingByte;
};

#define PT_BASE1    200
#define PT_SBYTE    (PT_BASE1 + 0)
#define PT_UBYTE    (PT_BASE1 + 1)
#define PT_SWORD    (PT_BASE1 + 2)
#define PT_UWORD    (PT_BASE1 + 3)
#define PT_SLONG    (PT_BASE1 + 4)
#define PT_ULONG    (PT_BASE1 + 5)
#define PT_BIT      (PT_BASE1 + 6)
#define PT_FLIPBIT  (PT_BASE1 + 7)
#define PT_EXISTS   (PT_BASE1 + 8)
#define PT_MISSING  (PT_BASE1 + 9)   /* deliberately absent from every
                                       * test tagList below -- its own
                                       * table entry must be found-
                                       * nothing, leave missingByte
                                       * untouched, and not be counted. */

#define PT_BASE2       300           /* reached only via PACK_NEWOFFSET() */
#define PT_BASE2FIELD  (PT_BASE2 + 0)

#define PTF_BIT      0x01   /* flags bit for PT_BIT (plain boolean) */
#define PTF_FLIPBIT  0x02   /* flags bit for PT_FLIPBIT (inverted boolean) */
#define PTF_EXISTS   0x04   /* flags bit for PT_EXISTS (existence hack) */

/* One shared, read-only pack table -- only the `pack`/`tagList`
 * arguments differ per call below. PSTF_EXISTS has no PKCTRL_*()
 * combination of its own in <utility/pack.h> (only PACK_ENTRY()/
 * PACK_BYTEBIT()'s `control` argument and the bare PSTF_EXISTS bit
 * constant exist), so the PT_EXISTS entry is built by hand from the
 * same PK_CALCOFFSET()/PK_BITNUM() building blocks PACK_BYTEBIT()
 * itself uses, with PSTF_EXISTS OR'd straight in -- not a hand-rolled
 * bit layout, the exact same macros the header's own PACK_BYTEBIT()
 * expands to. */
static ULONG g_packTable[] = {
    PACK_STARTTABLE(PT_BASE1),
    PACK_ENTRY(PT_BASE1, PT_SBYTE, PackTestStruct, sByte, PKCTRL_BYTE  | PKCTRL_PACKUNPACK),
    PACK_ENTRY(PT_BASE1, PT_UBYTE, PackTestStruct, uByte, PKCTRL_UBYTE | PKCTRL_PACKUNPACK),
    PACK_ENTRY(PT_BASE1, PT_SWORD, PackTestStruct, sWord, PKCTRL_WORD  | PKCTRL_PACKUNPACK),
    PACK_ENTRY(PT_BASE1, PT_UWORD, PackTestStruct, uWord, PKCTRL_UWORD | PKCTRL_PACKUNPACK),
    PACK_ENTRY(PT_BASE1, PT_SLONG, PackTestStruct, sLong, PKCTRL_LONG  | PKCTRL_PACKUNPACK),
    PACK_ENTRY(PT_BASE1, PT_ULONG, PackTestStruct, uLong, PKCTRL_ULONG | PKCTRL_PACKUNPACK),
    PACK_BYTEBIT(PT_BASE1, PT_BIT,     PackTestStruct, flags, PKCTRL_BIT     | PKCTRL_PACKUNPACK, PTF_BIT),
    PACK_BYTEBIT(PT_BASE1, PT_FLIPBIT, PackTestStruct, flags, PKCTRL_FLIPBIT | PKCTRL_PACKUNPACK, PTF_FLIPBIT),
    (PKCTRL_BIT | PKCTRL_PACKUNPACK | PSTF_EXISTS
        | ((PT_EXISTS - PT_BASE1) << 16L)
        | PK_CALCOFFSET(PackTestStruct, flags)
        | (PK_BITNUM(PTF_EXISTS) << 13L)),
    PACK_ENTRY(PT_BASE1, PT_MISSING, PackTestStruct, missingByte, PKCTRL_UBYTE | PKCTRL_PACKUNPACK),
    PACK_NEWOFFSET(PT_BASE2),
    PACK_ENTRY(PT_BASE2, PT_BASE2FIELD, PackTestStruct, chainByte, PKCTRL_UBYTE | PKCTRL_PACKUNPACK),
    PACK_ENDTABLE
};

static void test_pack_structure_tags_roundtrip(struct Library *newBase, struct Library *realBase)
{
    struct TagItem tagList[] = {
        { PT_SBYTE,       (ULONG)(LONG)-5 },
        { PT_UBYTE,       250 },
        { PT_SWORD,       (ULONG)(LONG)-12345 },
        { PT_UWORD,       54321 },
        { PT_SLONG,       (ULONG)(LONG)-123456789L },
        { PT_ULONG,       0xDEADBEEFUL },
        { PT_BASE2FIELD,  42 },
        { TAG_DONE,       0 },
    };
    struct PackTestStruct newStruct, realStruct;
    ULONG newNum, realNum;
    BOOL ok;

    newStruct.sByte = 0; newStruct.uByte = 0; newStruct.sWord = 0;
    newStruct.uWord = 0; newStruct.sLong = 0; newStruct.uLong = 0;
    newStruct.flags = 0; newStruct.chainByte = 0; newStruct.missingByte = 0xAA;
    realStruct = newStruct;

    newNum  = PackStructureTags_Call(newBase,  &newStruct,  g_packTable, tagList);
    realNum = PackStructureTags_Call(realBase, &realStruct, g_packTable, tagList);

    /* 7 tags present (SBYTE/UBYTE/SWORD/UWORD/SLONG/ULONG/BASE2FIELD,
     * the last reached only via the table's own PACK_NEWOFFSET()
     * chaining to PT_BASE2); PT_BIT/PT_FLIPBIT/PT_EXISTS (own dedicated
     * test below) and PT_MISSING (deliberately absent, must be
     * skipped and NOT counted) bring the table to 11 entries total. */
    ok = (newNum == realNum) && newNum == 7 &&
         newStruct.sByte       == realStruct.sByte       && newStruct.sByte       == -5 &&
         newStruct.uByte       == realStruct.uByte       && newStruct.uByte       == 250 &&
         newStruct.sWord       == realStruct.sWord       && newStruct.sWord       == -12345 &&
         newStruct.uWord       == realStruct.uWord       && newStruct.uWord       == 54321 &&
         newStruct.sLong       == realStruct.sLong       && newStruct.sLong       == -123456789L &&
         newStruct.uLong       == realStruct.uLong       && newStruct.uLong       == 0xDEADBEEFUL &&
         newStruct.chainByte   == realStruct.chainByte   && newStruct.chainByte   == 42 &&
         newStruct.missingByte == realStruct.missingByte && newStruct.missingByte == 0xAA &&
         newStruct.flags       == realStruct.flags       && newStruct.flags       == 0;

    result((CONST_STRPTR) "PackStructureTags-ByteWordLongSignedUnsignedRoundTripPlusChainedBaseTagPlusMissingSkipped", ok);
}

static void test_pack_structure_tags_bit(struct Library *newBase, struct Library *realBase)
{
    /* Set case: PT_BIT=TRUE (nonzero ti_Data) -> bit0 SET. PT_FLIPBIT=
     * TRUE -> inverted boolean -> bit1 CLEARED. Pre-seeding flags to
     * 0xFF (all bits set) so a "set" outcome and a "no entry touched
     * this bit" outcome are distinguishable from a "cleared" outcome --
     * and so the read-modify-write behaviour (only the addressed bit
     * changes, nothing else in the byte) is actually exercised. */
    struct TagItem setList[] = {
        { PT_BIT,     1 },
        { PT_FLIPBIT, 1 },
        { TAG_DONE,   0 },
    };
    /* Clear case: PT_BIT=FALSE -> bit0 CLEARED. PT_FLIPBIT=FALSE ->
     * inverted -> bit1 SET. Pre-seeded to 0x00 this time, for the same
     * reason in reverse. */
    struct TagItem clearList[] = {
        { PT_BIT,     0 },
        { PT_FLIPBIT, 0 },
        { TAG_DONE,   0 },
    };
    struct PackTestStruct newStruct, realStruct;
    ULONG newNum, realNum;
    BOOL ok;

    newStruct.sByte = 0; newStruct.uByte = 0; newStruct.sWord = 0;
    newStruct.uWord = 0; newStruct.sLong = 0; newStruct.uLong = 0;
    newStruct.flags = 0xFF; newStruct.chainByte = 0; newStruct.missingByte = 0;
    realStruct = newStruct;

    newNum  = PackStructureTags_Call(newBase,  &newStruct,  g_packTable, setList);
    realNum = PackStructureTags_Call(realBase, &realStruct, g_packTable, setList);

    /* bit0 (PTF_BIT) stays set, bit1 (PTF_FLIPBIT) gets cleared by the
     * inverted boolean, every other bit of the pre-seeded 0xFF survives
     * untouched -- expected 0xFF & ~PTF_FLIPBIT == 0xFD. */
    ok = (newNum == realNum) && newNum == 2 &&
         newStruct.flags == realStruct.flags && newStruct.flags == (UBYTE)(0xFF & ~PTF_FLIPBIT);

    if (ok) {
        newStruct.flags = 0x00; realStruct.flags = 0x00;

        newNum  = PackStructureTags_Call(newBase,  &newStruct,  g_packTable, clearList);
        realNum = PackStructureTags_Call(realBase, &realStruct, g_packTable, clearList);

        /* bit0 stays clear, bit1 gets SET by the inverted boolean --
         * expected PTF_FLIPBIT alone (0x02). */
        ok = (newNum == realNum) && newNum == 2 &&
             newStruct.flags == realStruct.flags && newStruct.flags == PTF_FLIPBIT;
    }

    result((CONST_STRPTR) "PackStructureTags-BitSetClearAndInvertedFlipBitPreservesOtherBits", ok);
}

static void test_pack_structure_tags_exists(struct Library *newBase, struct Library *realBase)
{
    /* The empirically-risky case this work package's own task brief
     * flags explicitly: PSTF_EXISTS set on a Bit-type entry. ti_Data is
     * deliberately 0 here -- this test's whole point is to settle,
     * against the real oracle, whether the bit gets set purely because
     * PT_EXISTS was FOUND in the taglist (this project's own reading of
     * <utility/pack.h>'s "1 = TAG_EXISTS is TRUE" comment) or whether
     * ti_Data's own (here falsy) value still matters despite that
     * comment. See provenance/utility-library-packtags.md for which of
     * the two this turned out to be. */
    struct TagItem tagList[] = {
        { PT_EXISTS, 0 },
        { TAG_DONE,  0 },
    };
    struct PackTestStruct newStruct, realStruct;
    ULONG newNum, realNum;
    BOOL ok;

    newStruct.sByte = 0; newStruct.uByte = 0; newStruct.sWord = 0;
    newStruct.uWord = 0; newStruct.sLong = 0; newStruct.uLong = 0;
    newStruct.flags = 0; newStruct.chainByte = 0; newStruct.missingByte = 0;
    realStruct = newStruct;

    newNum  = PackStructureTags_Call(newBase,  &newStruct,  g_packTable, tagList);
    realNum = PackStructureTags_Call(realBase, &realStruct, g_packTable, tagList);

    ok = (newNum == realNum) && newNum == 1 &&
         newStruct.flags == realStruct.flags;

    result((CONST_STRPTR) "PackStructureTags-ExistsHackIgnoresFalsyTiData", ok);
}

static void test_unpack_structure_tags_roundtrip(struct Library *newBase, struct Library *realBase)
{
    struct PackTestStruct src;
    ULONG newOut[7], realOut[7];
    struct TagItem newList[8], realList[8];
    ULONG newNum, realNum;
    ULONG i;
    BOOL ok;

    src.sByte = -5; src.uByte = 250;
    src.sWord = -12345; src.uWord = 54321;
    src.sLong = -123456789L; src.uLong = 0xDEADBEEFUL;
    src.flags = 0; src.chainByte = 42; src.missingByte = 0xAA;

    for (i = 0; i < 7; i++) {
        newOut[i] = 0xFFFFFFFFUL;
        realOut[i] = 0xFFFFFFFFUL;
    }

    newList[0].ti_Tag  = PT_SBYTE;      newList[0].ti_Data = (ULONG)(APTR)&newOut[0];
    newList[1].ti_Tag  = PT_UBYTE;      newList[1].ti_Data = (ULONG)(APTR)&newOut[1];
    newList[2].ti_Tag  = PT_SWORD;      newList[2].ti_Data = (ULONG)(APTR)&newOut[2];
    newList[3].ti_Tag  = PT_UWORD;      newList[3].ti_Data = (ULONG)(APTR)&newOut[3];
    newList[4].ti_Tag  = PT_SLONG;      newList[4].ti_Data = (ULONG)(APTR)&newOut[4];
    newList[5].ti_Tag  = PT_ULONG;      newList[5].ti_Data = (ULONG)(APTR)&newOut[5];
    newList[6].ti_Tag  = PT_BASE2FIELD; newList[6].ti_Data = (ULONG)(APTR)&newOut[6];
    newList[7].ti_Tag  = TAG_DONE;      newList[7].ti_Data = 0;

    realList[0].ti_Tag = PT_SBYTE;      realList[0].ti_Data = (ULONG)(APTR)&realOut[0];
    realList[1].ti_Tag = PT_UBYTE;      realList[1].ti_Data = (ULONG)(APTR)&realOut[1];
    realList[2].ti_Tag = PT_SWORD;      realList[2].ti_Data = (ULONG)(APTR)&realOut[2];
    realList[3].ti_Tag = PT_UWORD;      realList[3].ti_Data = (ULONG)(APTR)&realOut[3];
    realList[4].ti_Tag = PT_SLONG;      realList[4].ti_Data = (ULONG)(APTR)&realOut[4];
    realList[5].ti_Tag = PT_ULONG;      realList[5].ti_Data = (ULONG)(APTR)&realOut[5];
    realList[6].ti_Tag = PT_BASE2FIELD; realList[6].ti_Data = (ULONG)(APTR)&realOut[6];
    realList[7].ti_Tag = TAG_DONE;      realList[7].ti_Data = 0;

    newNum  = UnpackStructureTags_Call(newBase,  &src, g_packTable, newList);
    realNum = UnpackStructureTags_Call(realBase, &src, g_packTable, realList);

    ok = (newNum == realNum) && newNum == 7;
    for (i = 0; ok && i < 7; i++)
        ok = (newOut[i] == realOut[i]);

    if (ok) {
        /* Sign-extension on unpack for the signed Byte/Word entries,
         * zero-extension for the unsigned ones, Long passed through
         * untouched -- checked against hand-computed expected values,
         * not just new-vs-real agreement. */
        ok = newOut[0] == (ULONG)(LONG)-5 &&            /* sByte, signed */
             newOut[1] == 250 &&                         /* uByte, unsigned */
             newOut[2] == (ULONG)(LONG)-12345 &&          /* sWord, signed */
             newOut[3] == 54321 &&                        /* uWord, unsigned */
             newOut[4] == (ULONG)-123456789L &&            /* sLong */
             newOut[5] == 0xDEADBEEFUL &&                  /* uLong */
             newOut[6] == 42;                              /* chainByte, via PT_BASE2 */
    }

    result((CONST_STRPTR) "UnpackStructureTags-SignExtendedByteWordPlusLongPlusChainedBaseTag", ok);
}

/* Runs one UnpackStructureTags_Call() sub-case (PT_BIT/PT_FLIPBIT/
 * PT_EXISTS against a given struct-bit state) through both library
 * bases and folds the result into *ok. Shared by the two sub-cases
 * test_unpack_structure_tags_bit() exercises below -- see that
 * function's own comment for what each sub-case proves. */
static void unpackBitSubcase(struct Library *newBase, struct Library *realBase,
                              struct PackTestStruct *src, BOOL *ok)
{
    ULONG newOut[3], realOut[3];
    struct TagItem newList[4], realList[4];
    ULONG newNum, realNum;

    newOut[0] = 0xFFFFFFFFUL; newOut[1] = 0xFFFFFFFFUL; newOut[2] = 0xFFFFFFFFUL;
    realOut[0] = 0xFFFFFFFFUL; realOut[1] = 0xFFFFFFFFUL; realOut[2] = 0xFFFFFFFFUL;

    newList[0].ti_Tag  = PT_BIT;     newList[0].ti_Data  = (ULONG)(APTR)&newOut[0];
    newList[1].ti_Tag  = PT_FLIPBIT; newList[1].ti_Data  = (ULONG)(APTR)&newOut[1];
    newList[2].ti_Tag  = PT_EXISTS;  newList[2].ti_Data  = (ULONG)(APTR)&newOut[2];
    newList[3].ti_Tag  = TAG_DONE;   newList[3].ti_Data  = 0;

    realList[0].ti_Tag = PT_BIT;     realList[0].ti_Data = (ULONG)(APTR)&realOut[0];
    realList[1].ti_Tag = PT_FLIPBIT; realList[1].ti_Data = (ULONG)(APTR)&realOut[1];
    realList[2].ti_Tag = PT_EXISTS;  realList[2].ti_Data = (ULONG)(APTR)&realOut[2];
    realList[3].ti_Tag = TAG_DONE;   realList[3].ti_Data = 0;

    newNum  = UnpackStructureTags_Call(newBase,  src, g_packTable, newList);
    realNum = UnpackStructureTags_Call(realBase, src, g_packTable, realList);

    *ok = *ok && (newNum == realNum) && newNum == 3 &&
          newOut[0] == realOut[0] && newOut[1] == realOut[1] && newOut[2] == realOut[2];
}

static void test_unpack_structure_tags_bit(struct Library *newBase, struct Library *realBase)
{
    /* Bit-type UnpackStructureTags, genuinely NOT the mirror image of
     * Pack's own Bit-type handling -- see tags.c's own big comment on
     * UnpackStructureTags' Bit-type case for the full empirically-
     * derived rule (real V40 ROM data gathered via a temporary,
     * diagnostic-only report()-based probe during this work package's
     * authoring, then removed, per this project's established
     * discipline -- see provenance/utility-library-math64.md's own
     * "Errors and fixes" for the precedent): a Bit-type entry's
     * *ti_Data is left COMPLETELY UNTOUCHED on the real ROM in every
     * combination EXCEPT PSTF_EXISTS set AND the struct's own addressed
     * bit CLEAR -- and even then the ROM only ever writes FALSE (0),
     * never TRUE. `num` still counts every FindTagItem()-matched entry
     * regardless of whether anything was actually written.
     *
     * Two struct-bit states exercise every combination this table's
     * three Bit-type entries (PT_BIT plain, PT_FLIPBIT inverted,
     * PT_EXISTS existence-hack) can produce: */
    struct PackTestStruct src;
    BOOL ok = TRUE;

    src.sByte = 0; src.uByte = 0; src.sWord = 0; src.uWord = 0;
    src.sLong = 0; src.uLong = 0; src.chainByte = 0; src.missingByte = 0;

    /* Sub-case 1: PTF_BIT set, PTF_FLIPBIT/PTF_EXISTS clear. PT_EXISTS'
     * own addressed bit (PTF_EXISTS) is CLEAR here -- the one
     * combination that actually writes, confirmed to write FALSE (0)
     * on the real ROM despite "TAG_EXISTS is TRUE"'s own wording. */
    src.flags = PTF_BIT;
    unpackBitSubcase(newBase, realBase, &src, &ok);

    /* Sub-case 2: PTF_BIT and PTF_EXISTS both set. PT_EXISTS' own
     * addressed bit is now SET -- confirmed on the real ROM to go back
     * to a no-write no-op, same as the plain/inverted entries above. */
    src.flags = (UBYTE)(PTF_BIT | PTF_EXISTS);
    unpackBitSubcase(newBase, realBase, &src, &ok);

    result((CONST_STRPTR) "UnpackStructureTags-BitTypeOnlyWritesFalseForExistsPlusClearBit", ok);
}

/* ======================================================================
 * Ninth work package (provenance/utility-library-namedobject.md): the
 * full V39 NamedObject family. Each test below runs the SAME sequence of
 * calls against both newBase and realBase and requires BOTH to behave
 * the same way at every step (not that the two bases hand back identical
 * pointers -- they're two separate library instances with two separate
 * private namespaces, so pointer VALUES are never compared across
 * bases, only booleans/strings/behavioural outcomes).
 * ====================================================================== */

/* Dereferences two STRPTR values directly -- legitimate here because
 * this whole test program, both library bases, and every object either
 * hands back all live in the SAME flat, unprotected address space (bare-
 * metal AmigaOS, no MMU-enforced process boundary) -- same assumption
 * tagListMatchesFlat()/tagListsIdentical() above already rely on for
 * struct TagItem contents. */
static BOOL strEqualLocal(STRPTR a, STRPTR b)
{
    if (!a || !b)
        return (BOOL)(a == b);
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (BOOL)(*a == *b);
}

static void test_alloc_free_roundtrip(struct Library *newBase, struct Library *realBase)
{
    struct NamedObject *newObj, *realObj;
    STRPTR newName, realName;
    BOOL ok;

    newObj = (struct NamedObject *)AllocNamedObjectA_Call(newBase, (APTR) "RoundTrip", NULL);
    realObj = (struct NamedObject *)AllocNamedObjectA_Call(realBase, (APTR) "RoundTrip", NULL);

    ok = (BOOL)(newObj != NULL && realObj != NULL);

    newName = (STRPTR)NamedObjectName_Call(newBase, newObj);
    realName = (STRPTR)NamedObjectName_Call(realBase, realObj);
    ok = (BOOL)(ok && strEqualLocal(newName, (STRPTR) "RoundTrip")
                    && strEqualLocal(realName, (STRPTR) "RoundTrip"));

    /* Plain alloc, no ANO_UserSpace -- no_Object must be NULL on both. */
    ok = (BOOL)(ok && newObj->no_Object == NULL && realObj->no_Object == NULL);

    FreeNamedObject_Call(newBase, newObj);
    FreeNamedObject_Call(realBase, realObj);

    result((CONST_STRPTR) "AllocNamedObjectA-FreeNamedObject-RoundTrip", ok);
}

static void test_alloc_userspace(struct Library *newBase, struct Library *realBase)
{
    struct TagItem tags[] = {
        { ANO_UserSpace, 16 },
        { TAG_DONE, 0 },
    };
    struct NamedObject *newObj, *realObj;
    BOOL ok;

    newObj = (struct NamedObject *)AllocNamedObjectA_Call(newBase, (APTR) "UserSpaceTest", tags);
    realObj = (struct NamedObject *)AllocNamedObjectA_Call(realBase, (APTR) "UserSpaceTest", tags);

    ok = (BOOL)(newObj != NULL && realObj != NULL
                && newObj->no_Object != NULL && realObj->no_Object != NULL);

    /* no_Object must point at genuinely usable, long-word-aligned
     * memory of (at least) the requested size -- write and read back a
     * recognizable pattern across the whole requested span on both. */
    if (ok) {
        ULONG *np = (ULONG *)newObj->no_Object;
        ULONG *rp = (ULONG *)realObj->no_Object;

        np[0] = 0x11111111UL; np[1] = 0x22222222UL;
        np[2] = 0x33333333UL; np[3] = 0x44444444UL;
        rp[0] = 0x11111111UL; rp[1] = 0x22222222UL;
        rp[2] = 0x33333333UL; rp[3] = 0x44444444UL;

        ok = (BOOL)(np[0] == 0x11111111UL && np[3] == 0x44444444UL
                    && rp[0] == 0x11111111UL && rp[3] == 0x44444444UL
                    && (((ULONG)(APTR)newObj->no_Object) & 3UL) == 0
                    && (((ULONG)(APTR)realObj->no_Object) & 3UL) == 0);
    }

    FreeNamedObject_Call(newBase, newObj);
    FreeNamedObject_Call(realBase, realObj);

    result((CONST_STRPTR) "AllocNamedObjectA-AnoUserSpace-UsableAlignedMemory", ok);
}

static void test_namedobjectname_null(struct Library *newBase, struct Library *realBase)
{
    BOOL ok;

    ok = (BOOL)(NamedObjectName_Call(newBase, NULL) == 0
                && NamedObjectName_Call(realBase, NULL) == 0);

    result((CONST_STRPTR) "NamedObjectName-NullObjectReturnsNull", ok);
}

static void test_add_find_namespace_and_findall(struct Library *newBase, struct Library *realBase)
{
    struct TagItem nsTag[] = { { ANO_NameSpace, TRUE }, { TAG_DONE, 0 } };
    struct NamedObject *parentN, *childAn, *childBn;
    struct NamedObject *parentR, *childAr, *childBr;
    struct NamedObject *first, *second, *third;
    BOOL ok;

    parentN = (struct NamedObject *)AllocNamedObjectA_Call(newBase, (APTR) "Parent", nsTag);
    childAn = (struct NamedObject *)AllocNamedObjectA_Call(newBase, (APTR) "ChildA", NULL);
    childBn = (struct NamedObject *)AllocNamedObjectA_Call(newBase, (APTR) "ChildB", NULL);

    parentR = (struct NamedObject *)AllocNamedObjectA_Call(realBase, (APTR) "Parent", nsTag);
    childAr = (struct NamedObject *)AllocNamedObjectA_Call(realBase, (APTR) "ChildA", NULL);
    childBr = (struct NamedObject *)AllocNamedObjectA_Call(realBase, (APTR) "ChildB", NULL);

    ok = (BOOL)(parentN && childAn && childBn && parentR && childAr && childBr);

    ok = (BOOL)(ok
                && AddNamedObject_Call(newBase, parentN, childAn)
                && AddNamedObject_Call(newBase, parentN, childBn)
                && AddNamedObject_Call(realBase, parentR, childAr)
                && AddNamedObject_Call(realBase, parentR, childBr));

    /* Find by exact name. */
    first = (struct NamedObject *)FindNamedObject_Call(newBase, parentN, (APTR) "ChildA", NULL);
    ok = (BOOL)(ok && first == childAn);
    if (first)
        ReleaseNamedObject_Call(newBase, first);

    first = (struct NamedObject *)FindNamedObject_Call(realBase, parentR, (APTR) "ChildA", NULL);
    ok = (BOOL)(ok && first == childAr);
    if (first)
        ReleaseNamedObject_Call(realBase, first);

    /* name == NULL matches all; lastObject continues the search; running
     * off the end returns NULL. Exercised identically on both bases. */
    first = (struct NamedObject *)FindNamedObject_Call(newBase, parentN, NULL, NULL);
    second = (struct NamedObject *)FindNamedObject_Call(newBase, parentN, NULL, first);
    third = (struct NamedObject *)FindNamedObject_Call(newBase, parentN, NULL, second);
    ok = (BOOL)(ok && first && second && first != second && third == NULL);
    if (first)
        ReleaseNamedObject_Call(newBase, first);
    if (second)
        ReleaseNamedObject_Call(newBase, second);

    first = (struct NamedObject *)FindNamedObject_Call(realBase, parentR, NULL, NULL);
    second = (struct NamedObject *)FindNamedObject_Call(realBase, parentR, NULL, first);
    third = (struct NamedObject *)FindNamedObject_Call(realBase, parentR, NULL, second);
    ok = (BOOL)(ok && first && second && first != second && third == NULL);
    if (first)
        ReleaseNamedObject_Call(realBase, first);
    if (second)
        ReleaseNamedObject_Call(realBase, second);

    /* Teardown: unlink + fully release each child via RemNamedObject's
     * own immediate-zero path (each child's only remaining reference at
     * this point is the one AllocNamedObjectA's own implicit use left
     * behind), then free. */
    {
        struct MsgPort *port = CreateMsgPort();
        struct Message msg;

        msg.mn_ReplyPort = port;
        msg.mn_Length = sizeof(msg);

        RemNamedObject_Call(newBase, childAn, &msg);
        WaitPort(port); GetMsg(port);
        RemNamedObject_Call(newBase, childBn, &msg);
        WaitPort(port); GetMsg(port);
        RemNamedObject_Call(realBase, childAr, &msg);
        WaitPort(port); GetMsg(port);
        RemNamedObject_Call(realBase, childBr, &msg);
        WaitPort(port); GetMsg(port);

        DeleteMsgPort(port);
    }
    FreeNamedObject_Call(newBase, childAn);
    FreeNamedObject_Call(newBase, childBn);
    FreeNamedObject_Call(newBase, parentN);
    FreeNamedObject_Call(realBase, childAr);
    FreeNamedObject_Call(realBase, childBr);
    FreeNamedObject_Call(realBase, parentR);

    result((CONST_STRPTR) "AddNamedObject-FindNamedObject-NamespaceAndFindAll", ok);
}

static void test_add_root_namespace(struct Library *newBase, struct Library *realBase)
{
    struct NamedObject *newObj, *realObj, *foundN, *foundR;
    struct MsgPort *port;
    struct Message msg;
    BOOL ok;

    newObj = (struct NamedObject *)AllocNamedObjectA_Call(newBase, (APTR) "RootDweller", NULL);
    realObj = (struct NamedObject *)AllocNamedObjectA_Call(realBase, (APTR) "RootDweller", NULL);

    ok = (BOOL)(newObj && realObj
                && AddNamedObject_Call(newBase, NULL, newObj)
                && AddNamedObject_Call(realBase, NULL, realObj));

    foundN = (struct NamedObject *)FindNamedObject_Call(newBase, NULL, (APTR) "RootDweller", NULL);
    foundR = (struct NamedObject *)FindNamedObject_Call(realBase, NULL, (APTR) "RootDweller", NULL);
    ok = (BOOL)(ok && foundN == newObj && foundR == realObj);

    if (foundN)
        ReleaseNamedObject_Call(newBase, foundN);
    if (foundR)
        ReleaseNamedObject_Call(realBase, foundR);

    /* Immediate-removal path: at this point each object's only remaining
     * reference is AllocNamedObjectA's own implicit use, so RemNamedObject
     * should unlink it, drop the count to zero, and reply the message
     * right away with ln_Name == the object -- verified via a REAL
     * MsgPort/PutMsg(inside ReplyMsg)/WaitPort/GetMsg round trip, not
     * just inspecting the struct's fields without using the port. */
    port = CreateMsgPort();
    msg.mn_ReplyPort = port;
    msg.mn_Length = sizeof(msg);
    msg.mn_Node.ln_Name = NULL;

    RemNamedObject_Call(newBase, newObj, &msg);
    /* This is the IMMEDIATE-reply case (useCount drops to 0 inside
     * RemNamedObject's own call, nobody else held a reference) -- the
     * reply has already landed on the port's message list by the time
     * RemNamedObject_Call() returns, so WaitPort() below must return
     * without blocking. (Deliberately NOT probing with a GetMsg() here
     * first: GetMsg() actually DEQUEUES, so an early probe would consume
     * the one-and-only reply and starve the WaitPort() below --
     * confirmed the hard way under volamos, which reported "would block
     * forever" until this ordering bug in the test itself was fixed.) */
    WaitPort(port);
    {
        struct Message *reply = GetMsg(port);
        ok = (BOOL)(ok && reply == &msg && reply->mn_Node.ln_Name == (char *)newObj);
    }

    msg.mn_Node.ln_Name = NULL;
    RemNamedObject_Call(realBase, realObj, &msg);
    WaitPort(port);
    {
        struct Message *reply = GetMsg(port);
        ok = (BOOL)(ok && reply == &msg && reply->mn_Node.ln_Name == (char *)realObj);
    }

    DeleteMsgPort(port);

    /* Confirm it's really gone from the namespace: a fresh find fails. */
    ok = (BOOL)(ok
                && FindNamedObject_Call(newBase, NULL, (APTR) "RootDweller", NULL) == 0
                && FindNamedObject_Call(realBase, NULL, (APTR) "RootDweller", NULL) == 0);

    FreeNamedObject_Call(newBase, newObj);
    FreeNamedObject_Call(realBase, realObj);

    result((CONST_STRPTR) "AddNamedObject-RootNamespace-RemNamedObject-ImmediateReply", ok);
}

static void test_nodups_rejected(struct Library *newBase, struct Library *realBase)
{
    struct TagItem nsTag[] = { { ANO_NameSpace, TRUE }, { ANO_Flags, NSF_NODUPS }, { TAG_DONE, 0 } };
    struct NamedObject *parentN, *dup1n, *dup2n;
    struct NamedObject *parentR, *dup1r, *dup2r;
    BOOL ok;
    struct MsgPort *port;
    struct Message msg;

    parentN = (struct NamedObject *)AllocNamedObjectA_Call(newBase, (APTR) "NoDupsParent", nsTag);
    report((CONST_STRPTR) "DIAG nodups 1 allocParentN\r\n", NULL);
    dup1n = (struct NamedObject *)AllocNamedObjectA_Call(newBase, (APTR) "Dup", NULL);
    report((CONST_STRPTR) "DIAG nodups 2 allocDup1n\r\n", NULL);
    dup2n = (struct NamedObject *)AllocNamedObjectA_Call(newBase, (APTR) "Dup", NULL);
    report((CONST_STRPTR) "DIAG nodups 3 allocDup2n\r\n", NULL);

    parentR = (struct NamedObject *)AllocNamedObjectA_Call(realBase, (APTR) "NoDupsParent", nsTag);
    report((CONST_STRPTR) "DIAG nodups 4 allocParentR\r\n", NULL);
    dup1r = (struct NamedObject *)AllocNamedObjectA_Call(realBase, (APTR) "Dup", NULL);
    report((CONST_STRPTR) "DIAG nodups 5 allocDup1r\r\n", NULL);
    dup2r = (struct NamedObject *)AllocNamedObjectA_Call(realBase, (APTR) "Dup", NULL);
    report((CONST_STRPTR) "DIAG nodups 6 allocDup2r\r\n", NULL);

    ok = (BOOL)(parentN && dup1n && dup2n && parentR && dup1r && dup2r);

    ok = (BOOL)(ok && AddNamedObject_Call(newBase, parentN, dup1n) != 0);
    report((CONST_STRPTR) "DIAG nodups 7 addDup1n\r\n", NULL);
    ok = (BOOL)(ok && AddNamedObject_Call(newBase, parentN, dup2n) == 0); /* rejected */
    report((CONST_STRPTR) "DIAG nodups 8 addDup2n\r\n", NULL);
    ok = (BOOL)(ok && AddNamedObject_Call(realBase, parentR, dup1r) != 0);
    report((CONST_STRPTR) "DIAG nodups 9 addDup1r\r\n", NULL);
    ok = (BOOL)(ok && AddNamedObject_Call(realBase, parentR, dup2r) == 0); /* rejected */
    report((CONST_STRPTR) "DIAG nodups 10 addDup2r\r\n", NULL);

    /* dup2* were never linked into anything -- plain FreeNamedObject. */
    FreeNamedObject_Call(newBase, dup2n);
    report((CONST_STRPTR) "DIAG nodups 11 freeDup2n\r\n", NULL);
    FreeNamedObject_Call(realBase, dup2r);
    report((CONST_STRPTR) "DIAG nodups 12 freeDup2r\r\n", NULL);

    port = CreateMsgPort();
    msg.mn_ReplyPort = port;
    msg.mn_Length = sizeof(msg);
    report((CONST_STRPTR) "DIAG nodups 13 createMsgPort\r\n", NULL);

    RemNamedObject_Call(newBase, dup1n, &msg);
    report((CONST_STRPTR) "DIAG nodups 14 remDup1nCalled\r\n", NULL);
    WaitPort(port);
    report((CONST_STRPTR) "DIAG nodups 15 remDup1nWaited\r\n", NULL);
    GetMsg(port);
    report((CONST_STRPTR) "DIAG nodups 16 remDup1nGot\r\n", NULL);
    RemNamedObject_Call(realBase, dup1r, &msg);
    report((CONST_STRPTR) "DIAG nodups 17 remDup1rCalled\r\n", NULL);
    WaitPort(port);
    report((CONST_STRPTR) "DIAG nodups 18 remDup1rWaited\r\n", NULL);
    GetMsg(port);
    report((CONST_STRPTR) "DIAG nodups 19 remDup1rGot\r\n", NULL);

    DeleteMsgPort(port);

    FreeNamedObject_Call(newBase, dup1n);
    FreeNamedObject_Call(newBase, parentN);
    FreeNamedObject_Call(realBase, dup1r);
    FreeNamedObject_Call(realBase, parentR);

    result((CONST_STRPTR) "AddNamedObject-NsfNoDups-DuplicateRejected", ok);
}

static void test_attemptrem_never_added_fails(struct Library *newBase, struct Library *realBase)
{
    struct NamedObject *newObj, *realObj, *foundN, *foundR;
    BOOL ok;

    newObj = (struct NamedObject *)AllocNamedObjectA_Call(newBase, (APTR) "AttemptTest", NULL);
    realObj = (struct NamedObject *)AllocNamedObjectA_Call(realBase, (APTR) "AttemptTest", NULL);
    ok = (BOOL)(newObj && realObj);

    /* FindNamedObject(nameSpace=NULL,...) searches the ROOT namespace --
     * newObj/realObj were never added to any namespace, so this must
     * fail to find them (NULL). Confirms root-namespace search doesn't
     * somehow pick up un-added objects. */
    foundN = (struct NamedObject *)FindNamedObject_Call(newBase, NULL, (APTR) "AttemptTest", NULL);
    foundR = (struct NamedObject *)FindNamedObject_Call(realBase, NULL, (APTR) "AttemptTest", NULL);
    ok = (BOOL)(ok && foundN == NULL && foundR == NULL);

    /* Real-oracle-confirmed correction (this work package's own golden-
     * test recording, real Kickstart 3.1): AttemptRemNamedObject on an
     * object that was NEVER AddNamedObject()ed to any namespace fails
     * (real ROM returns 0/FALSE here -- diagnosed via a temporary raw-
     * value probe before this assertion was written: "DIAG
     * AttemptRemNamedObject-NeverAdded new=1 real=0", the new=1 being
     * this project's own first-draft bug). "Attempts to remove an object
     * from whatever NameSpace it's in" is read literally: there has to
     * BE a namespace it's in for this to mean anything, independent of
     * use count. Fixed in namedobject.c's AttemptRemNamedObject via an
     * explicit `inNamespace` check. */
    ok = (BOOL)(ok
                && AttemptRemNamedObject_Call(newBase, newObj) == 0
                && AttemptRemNamedObject_Call(realBase, realObj) == 0);

    FreeNamedObject_Call(newBase, newObj);
    FreeNamedObject_Call(realBase, realObj);

    result((CONST_STRPTR) "AttemptRemNamedObject-NeverAddedToNamespace-Fails", ok);
}

/* ------------------------------------------------------------------------
 * Provenance-integrity correction (2026-10-13, found in review -- see
 * provenance/utility-library-namedobject.md's dated correction section):
 * namedobject.c's own "Design decision 1" comment used to claim a test
 * named `NamedObject-UseAsNamespaceWithoutAnoNameSpaceFlag` confirmed the
 * real ROM permits using a flagless (no ANO_NameSpace) object as someone
 * else's `nameSpace` argument. That test never existed. A later,
 * unrelated follow-up work package's own concurrency test setup
 * accidentally exercised this exact case against real Kickstart 3.1 and
 * found the OPPOSITE: every AddNamedObject() call against such an object
 * failed outright (`okA=0 okB=0`). This dedicated differential test
 * exercises it directly and deliberately, matching namedobject.c's now-
 * fixed AddNamedObject() precondition check.
 * ------------------------------------------------------------------------ */
static void test_addnamedobject_nonnamespace_object(struct Library *newBase, struct Library *realBase)
{
    struct NamedObject *nsN, *nsR, *objN, *objR, *foundN, *foundR;
    ULONG addN, addR, findN, findR;
    ULONG diagArgs[2];
    BOOL ok;

    /* Candidate "namespace" objects allocated WITHOUT ANO_NameSpace --
     * deliberately NOT given ANO_NameSpace=TRUE, unlike every other
     * object in this file ever used as a `nameSpace` argument. */
    nsN  = (struct NamedObject *)AllocNamedObjectA_Call(newBase,  (APTR) "NotANamespace", NULL);
    nsR  = (struct NamedObject *)AllocNamedObjectA_Call(realBase, (APTR) "NotANamespace", NULL);
    objN = (struct NamedObject *)AllocNamedObjectA_Call(newBase,  (APTR) "Orphan", NULL);
    objR = (struct NamedObject *)AllocNamedObjectA_Call(realBase, (APTR) "Orphan", NULL);
    report((CONST_STRPTR) "DIAG addnonns 1 alloced\r\n", NULL);

    ok = (BOOL)(nsN && nsR && objN && objR);

    /* Raw, unconditional probe first -- same house style as the earlier
     * AttemptRemNamedObject-NeverAdded diagnosis: observe both bases'
     * actual D0 return value before asserting anything about it.
     * MASKED to 16 bits before use as a strict boolean: BOOL is
     * documented as exactly 2 bytes wide on this ABI (exec/types.h:
     * "typedef short BOOL ... sizeof(BOOL) == 2"), so only D0's low
     * word is a meaningful part of a BOOL-returning function's result
     * -- the high word is NOT guaranteed clear by the ABI, merely
     * happened to read as clear for every OTHER BOOL call site this
     * test program already relies on (confirmed empirically: this
     * exact call's own first recording read back D0 == 0x00020000,
     * high word nonzero, low word correctly 0 -- an "&1==0"-shaped mask
     * everywhere a xxx_Call() wrapper's result is compared strictly to
     * 0/nonzero for a BOOL-returning function, not a library bug). */
    addN = AddNamedObject_Call(newBase, nsN, objN) & 0xFFFFUL;
    addR = AddNamedObject_Call(realBase, nsR, objR) & 0xFFFFUL;
    diagArgs[0] = addN;
    diagArgs[1] = addR;
    report((CONST_STRPTR) "DIAG AddNamedObject-NonNamespace new=%lu real=%lu\r\n", diagArgs);

    /* Real-oracle-confirmed: AddNamedObject against a `nameSpace` that
     * was never given ANO_NameSpace=TRUE fails outright on real
     * Kickstart 3.1. namedobject.c's AddNamedObject() is now fixed to
     * match. */
    ok = (BOOL)(ok && addN == 0 && addR == 0);
    report((CONST_STRPTR) "DIAG addnonns 2 firstcheck\r\n", NULL);

    /* No side effects from the rejected call: objN/objR must still be
     * perfectly usable -- specifically, adding them into the (real) ROOT
     * namespace afterward must still succeed, proving the rejected call
     * didn't leave inNamespace/useCount/anything else half-mutated.
     * (Pure truthy use here, not a strict ==0/!=0 compare, so the BOOL
     * high-word caveat above doesn't apply -- TRUE's own existing
     * codegen already clears the full register via moveq, confirmed by
     * inspection of the compiled output.) */
    ok = (BOOL)(ok
                && AddNamedObject_Call(newBase, NULL, objN)
                && AddNamedObject_Call(realBase, NULL, objR));
    report((CONST_STRPTR) "DIAG addnonns 3 rootadd\r\n", NULL);

    foundN = (struct NamedObject *)FindNamedObject_Call(newBase, NULL, (APTR) "Orphan", NULL);
    foundR = (struct NamedObject *)FindNamedObject_Call(realBase, NULL, (APTR) "Orphan", NULL);
    ok = (BOOL)(ok && foundN == objN && foundR == objR);
    report((CONST_STRPTR) "DIAG addnonns 4 rootfind\r\n", NULL);
    if (foundN)
        ReleaseNamedObject_Call(newBase, foundN);
    if (foundR)
        ReleaseNamedObject_Call(realBase, foundR);
    report((CONST_STRPTR) "DIAG addnonns 5 released\r\n", NULL);

    /* Also confirm the rejected call never actually enqueued anything
     * into nsN/nsR's own children list at all: a name==NULL (matches
     * all) FindNamedObject against the non-namespace object must find
     * nothing. FindNamedObject returns a full-width pointer (LONG-sized,
     * not BOOL), so no masking is needed here. */
    findN = FindNamedObject_Call(newBase, nsN, NULL, NULL);
    findR = FindNamedObject_Call(realBase, nsR, NULL, NULL);
    diagArgs[0] = findN;
    diagArgs[1] = findR;
    report((CONST_STRPTR) "DIAG FindNamedObject-NonNamespace-EmptyAfterRejectedAdd new=%lu real=%lu\r\n", diagArgs);
    ok = (BOOL)(ok && findN == 0 && findR == 0);
    report((CONST_STRPTR) "DIAG addnonns 6 emptycheck\r\n", NULL);

    /* Teardown: unlink objN/objR from the root namespace they were
     * legitimately added to above, then free everything. Densely
     * DIAG-reported throughout -- this test runs immediately after the
     * locking follow-up's own two CreateTask()-based concurrency
     * sub-tests, in the same real-rig region that work package's own
     * provenance already documents as needing exactly this kind of
     * "breathing room" to avoid a silent, unexplained stall (see
     * provenance/utility-library-namedobject.md's own dated section on
     * this), so each step is reported individually here too rather than
     * assumed safe just because the equivalent sequence in
     * test_add_root_namespace() above runs cleanly on its own. */
    {
        struct MsgPort *port = CreateMsgPort();
        struct Message msg;

        report((CONST_STRPTR) "DIAG addnonns 7 portcreated\r\n", NULL);

        msg.mn_ReplyPort = port;
        msg.mn_Length = sizeof(msg);

        RemNamedObject_Call(newBase, objN, &msg);
        report((CONST_STRPTR) "DIAG addnonns 8 remNCalled\r\n", NULL);
        WaitPort(port);
        report((CONST_STRPTR) "DIAG addnonns 9 remNWaited\r\n", NULL);
        GetMsg(port);
        report((CONST_STRPTR) "DIAG addnonns 10 remNGot\r\n", NULL);

        RemNamedObject_Call(realBase, objR, &msg);
        report((CONST_STRPTR) "DIAG addnonns 11 remRCalled\r\n", NULL);
        WaitPort(port);
        report((CONST_STRPTR) "DIAG addnonns 12 remRWaited\r\n", NULL);
        GetMsg(port);
        report((CONST_STRPTR) "DIAG addnonns 13 remRGot\r\n", NULL);

        DeleteMsgPort(port);
        report((CONST_STRPTR) "DIAG addnonns 14 portdeleted\r\n", NULL);
    }
    FreeNamedObject_Call(newBase, objN);
    FreeNamedObject_Call(realBase, objR);
    FreeNamedObject_Call(newBase, nsN);
    FreeNamedObject_Call(realBase, nsR);
    report((CONST_STRPTR) "DIAG addnonns 15 freed\r\n", NULL);

    result((CONST_STRPTR) "AddNamedObject-NonNamespaceObject-FailsNoSideEffects", ok);
}

/* ------------------------------------------------------------------------
 * Companion to the test above: does FindNamedObject() enforce the SAME
 * ANO_NameSpace precondition as AddNamedObject(), or does it behave
 * differently (e.g. simply finding nothing in an empty/absent children
 * list rather than failing outright)? Not assumed from AddNamedObject's
 * own answer -- this project's own established practice (MapTags,
 * SMult64, the AttemptRemNamedObject/RemNamedObject removed-latch fixes,
 * all elsewhere in this library) has repeatedly found that superficially
 * similar functions don't always share edge-case behaviour, tested
 * directly rather than assumed. nsN/nsR here are allocated WITHOUT
 * ANO_NameSpace and NEVER have anything AddNamedObject()ed into them by
 * any path (AddNamedObject itself now rejects that, per the fix above),
 * so this call exercises FindNamedObject() reading whatever this
 * object's own private "am I a namespace" state looks like, on both
 * bases, in isolation from the AddNamedObject question above. The two
 * DIAG "...-done" lines below exist so a real crash/hang on EITHER base
 * shows up as a truncated serial transcript at a known point, rather
 * than silently reporting nothing at all.
 * ------------------------------------------------------------------------ */
static void test_findnamedobject_nonnamespace_object(struct Library *newBase, struct Library *realBase)
{
    struct NamedObject *nsN, *nsR;
    ULONG findN, findR;
    ULONG diagArgs[2];
    BOOL ok;

    nsN = (struct NamedObject *)AllocNamedObjectA_Call(newBase,  (APTR) "NeverANamespace", NULL);
    nsR = (struct NamedObject *)AllocNamedObjectA_Call(realBase, (APTR) "NeverANamespace", NULL);
    report((CONST_STRPTR) "DIAG findnonns 1 alloced\r\n", NULL);
    ok = (BOOL)(nsN && nsR);

    findN = FindNamedObject_Call(newBase, nsN, NULL, NULL);
    report((CONST_STRPTR) "DIAG FindNamedObject-NonNamespace new-done\r\n", NULL);
    findR = FindNamedObject_Call(realBase, nsR, NULL, NULL);
    report((CONST_STRPTR) "DIAG FindNamedObject-NonNamespace real-done\r\n", NULL);

    diagArgs[0] = findN;
    diagArgs[1] = findR;
    report((CONST_STRPTR) "DIAG FindNamedObject-NonNamespace new=%lu real=%lu\r\n", diagArgs);

    /* Both must return NULL -- whether via an explicit precondition
     * check mirroring AddNamedObject's, or simply finding nothing in an
     * empty children list, the user-visible result for this probe is
     * the same either way; see this function's own header comment for
     * why the exact mechanism still matters and was checked, not
     * assumed. */
    ok = (BOOL)(ok && findN == 0 && findR == 0);
    report((CONST_STRPTR) "DIAG findnonns 2 checked\r\n", NULL);

    FreeNamedObject_Call(newBase, nsN);
    FreeNamedObject_Call(realBase, nsR);
    report((CONST_STRPTR) "DIAG findnonns 3 freed\r\n", NULL);

    result((CONST_STRPTR) "FindNamedObject-NonNamespaceObject-ReturnsNull", ok);
}

static void test_attemptrem_success_after_add_and_release(struct Library *newBase,
                                                            struct Library *realBase)
{
    struct TagItem nsTag[] = { { ANO_NameSpace, TRUE }, { TAG_DONE, 0 } };
    struct NamedObject *parentN, *objN, *foundN;
    struct NamedObject *parentR, *objR, *foundR;
    BOOL ok;

    parentN = (struct NamedObject *)AllocNamedObjectA_Call(newBase, (APTR) "AttemptParent", nsTag);
    objN = (struct NamedObject *)AllocNamedObjectA_Call(newBase, (APTR) "AttemptSucceed", NULL);
    parentR = (struct NamedObject *)AllocNamedObjectA_Call(realBase, (APTR) "AttemptParent", nsTag);
    objR = (struct NamedObject *)AllocNamedObjectA_Call(realBase, (APTR) "AttemptSucceed", NULL);

    ok = (BOOL)(parentN && objN && parentR && objR
                && AddNamedObject_Call(newBase, parentN, objN)
                && AddNamedObject_Call(realBase, parentR, objR));

    /* Bump the use count via a real Find (now the object genuinely IS
     * in a namespace); confirm "in use by someone else" still blocks
     * AttemptRemNamedObject (and doesn't touch anything), then release
     * that find back down to just the alloc's own reference and confirm
     * AttemptRemNamedObject now succeeds immediately. */
    foundN = (struct NamedObject *)FindNamedObject_Call(newBase, parentN, (APTR) "AttemptSucceed", NULL);
    foundR = (struct NamedObject *)FindNamedObject_Call(realBase, parentR, (APTR) "AttemptSucceed", NULL);
    ok = (BOOL)(ok && foundN == objN && foundR == objR);

    ok = (BOOL)(ok
                && AttemptRemNamedObject_Call(newBase, objN) == 0
                && AttemptRemNamedObject_Call(realBase, objR) == 0);

    ReleaseNamedObject_Call(newBase, objN);
    ReleaseNamedObject_Call(realBase, objR);

    ok = (BOOL)(ok
                && AttemptRemNamedObject_Call(newBase, objN) != 0
                && AttemptRemNamedObject_Call(realBase, objR) != 0);

    /* A second AttemptRemNamedObject on an already-fully-removed object
     * must FAIL on the real ROM -- also confirmed the hard way, this
     * project's own first draft instead re-"succeeded" a second time
     * (useCount already read 0, so its old "<=1 means remove" logic
     * fired again). See namedobject.c's own design-decision-2 comment
     * for the fix (a `removed` latch, independent of whatever useCount
     * happens to read). */
    ok = (BOOL)(ok
                && AttemptRemNamedObject_Call(newBase, objN) == 0
                && AttemptRemNamedObject_Call(realBase, objR) == 0);

    FreeNamedObject_Call(newBase, objN);
    FreeNamedObject_Call(newBase, parentN);
    FreeNamedObject_Call(realBase, objR);
    FreeNamedObject_Call(realBase, parentR);

    result((CONST_STRPTR) "AttemptRemNamedObject-SucceedsAfterAddAndRelease-ThenFailsOnRepeat", ok);
}

static void test_attemptrem_blocked_while_found(struct Library *newBase, struct Library *realBase)
{
    struct TagItem nsTag[] = { { ANO_NameSpace, TRUE }, { TAG_DONE, 0 } };
    struct NamedObject *parentN, *objN, *foundN;
    struct NamedObject *parentR, *objR, *foundR;
    BOOL ok;
    struct MsgPort *port;
    struct Message msg;

    parentN = (struct NamedObject *)AllocNamedObjectA_Call(newBase, (APTR) "BlockParent", nsTag);
    objN = (struct NamedObject *)AllocNamedObjectA_Call(newBase, (APTR) "Blocked", NULL);
    parentR = (struct NamedObject *)AllocNamedObjectA_Call(realBase, (APTR) "BlockParent", nsTag);
    objR = (struct NamedObject *)AllocNamedObjectA_Call(realBase, (APTR) "Blocked", NULL);

    ok = (BOOL)(parentN && objN && parentR && objR
                && AddNamedObject_Call(newBase, parentN, objN)
                && AddNamedObject_Call(realBase, parentR, objR));

    foundN = (struct NamedObject *)FindNamedObject_Call(newBase, parentN, (APTR) "Blocked", NULL);
    foundR = (struct NamedObject *)FindNamedObject_Call(realBase, parentR, (APTR) "Blocked", NULL);
    ok = (BOOL)(ok && foundN == objN && foundR == objR);

    /* Use count is now 2 (alloc + this find) -- AttemptRemNamedObject
     * must fail WITHOUT consuming anything, since it can't complete
     * synchronously while someone else (the find above) still holds a
     * reference. */
    ok = (BOOL)(ok
                && AttemptRemNamedObject_Call(newBase, objN) == 0
                && AttemptRemNamedObject_Call(realBase, objR) == 0);

    /* Confirm the attempt really didn't touch the count: the object must
     * still be findable/usable, and releasing the outstanding find
     * reference plus the alloc's own reference (two releases) must
     * cleanly bring it to zero -- exercised via RemNamedObject on the
     * find's own reference (replicating "you must have found the object
     * first" for the call that actually drives removal), then one
     * ReleaseNamedObject for the alloc's leftover reference. */
    {
        port = CreateMsgPort();
        msg.mn_ReplyPort = port;
        msg.mn_Length = sizeof(msg);
        msg.mn_Node.ln_Name = NULL;

        RemNamedObject_Call(newBase, foundN, &msg);
        /* useCount was 2; RemNamedObject's own internal release brings it
         * to 1 (the alloc's leftover reference) -- must NOT reply yet. */
        ok = (BOOL)(ok && GetMsg(port) == NULL);
        ReleaseNamedObject_Call(newBase, objN); /* alloc's own reference -> 0, replies now */
        WaitPort(port);
        {
            struct Message *reply = GetMsg(port);
            ok = (BOOL)(ok && reply == &msg && reply->mn_Node.ln_Name == (char *)objN);
        }

        msg.mn_Node.ln_Name = NULL;
        RemNamedObject_Call(realBase, foundR, &msg);
        ok = (BOOL)(ok && GetMsg(port) == NULL);
        ReleaseNamedObject_Call(realBase, objR);
        WaitPort(port);
        {
            struct Message *reply = GetMsg(port);
            ok = (BOOL)(ok && reply == &msg && reply->mn_Node.ln_Name == (char *)objR);
        }

        DeleteMsgPort(port);
    }

    FreeNamedObject_Call(newBase, objN);
    FreeNamedObject_Call(newBase, parentN);
    FreeNamedObject_Call(realBase, objR);
    FreeNamedObject_Call(realBase, parentR);

    result((CONST_STRPTR) "AttemptRemNamedObject-BlockedWhileFound-ThenCleanRelease", ok);
}

/* Bounded wait for a message on `port`, up to `seconds` -- used ONLY for
 * the repeat-RemNamedObject-on-an-already-removed-object probe below,
 * where whether the real ROM replies at ALL is exactly the open question
 * (see provenance/utility-library-namedobject.md's follow-up section).
 * A plain WaitPort() would hang forever if the real ROM's behaviour here
 * turns out to be "never reply" -- same timer.device/UNIT_VBLANK technique
 * already established in src/c/wait.c's own wait_ticks(), multiplexed via
 * Wait() against the port's own signal bit instead of SIGBREAKF_CTRL_C.
 * Returns the message if one arrived in time, else NULL (and cleans up
 * the timer request either way). */
static struct Message *WaitPortTimeout(struct MsgPort *port, ULONG seconds)
{
    struct MsgPort *timerPort;
    struct timerequest *tr;
    struct Message *msg = NULL;

    msg = GetMsg(port);
    if (msg)
        return msg;

    timerPort = CreateMsgPort();
    if (!timerPort)
        return NULL;

    tr = (struct timerequest *)CreateIORequest(timerPort, sizeof(struct timerequest));
    if (!tr) {
        DeleteMsgPort(timerPort);
        return NULL;
    }

    if (OpenDevice((STRPTR)TIMERNAME, UNIT_VBLANK, (struct IORequest *)tr, 0) == 0) {
        ULONG sigs;

        tr->tr_node.io_Command = TR_ADDREQUEST;
        tr->tr_time.tv_secs = seconds;
        tr->tr_time.tv_micro = 0;
        SendIO((struct IORequest *)tr);

        sigs = Wait((1UL << port->mp_SigBit) | (1UL << timerPort->mp_SigBit));

        if (sigs & (1UL << port->mp_SigBit))
            msg = GetMsg(port);

        if (!CheckIO((struct IORequest *)tr))
            AbortIO((struct IORequest *)tr);
        WaitIO((struct IORequest *)tr);

        /* The timer signal might have arrived in the SAME Wait() as the
         * port's own -- re-check non-blockingly in case the port signal
         * bit was set but this is the first time we've drained it. */
        if (!msg)
            msg = GetMsg(port);

        CloseDevice((struct IORequest *)tr);
    }

    DeleteIORequest((struct IORequest *)tr);
    DeleteMsgPort(timerPort);

    return msg;
}

/* ------------------------------------------------------------------------
 * RemNamedObject-RepeatOnAlreadyRemoved -- real-oracle probe for a bug
 * found in review (not yet oracle-tested either way), see
 * provenance/utility-library-namedobject.md's follow-up section.
 *
 * AttemptRemNamedObject() was hardened with a `removed` latch specifically
 * because a SECOND call on an already-fully-removed object was found,
 * via real-rig testing, to incorrectly re-succeed if the check only
 * looked at useCount. RemNamedObject() was given the exact same
 * removed/useCount/pendingRemoval state machine but was NEVER given the
 * equivalent guard -- it has no check for `removed` at the top of the
 * function at all. This test fully removes an object via ONE
 * RemNamedObject() call (immediate-reply path, nobody else holds a
 * reference), then calls RemNamedObject() AGAIN on the very same object
 * with a FRESH message, and observes what each base actually does:
 * does the second message get replied at all, and if so, with what
 * ln_Name? WaitPortTimeout() bounds the wait so a real "never reply"
 * (Guru/hang-equivalent-at-the-library-level) answer shows up as an
 * observed timeout rather than hanging this whole test program forever.
 * ------------------------------------------------------------------------ */
static void test_remnamedobject_repeat_on_already_removed(struct Library *newBase,
                                                            struct Library *realBase)
{
    struct NamedObject *newObj, *realObj;
    struct MsgPort *port;
    struct Message msg1, msg2;
    struct Message *reply1N, *reply2N, *reply1R, *reply2R;
    BOOL ok;

    newObj = (struct NamedObject *)AllocNamedObjectA_Call(newBase, (APTR) "RepeatRemove", NULL);
    realObj = (struct NamedObject *)AllocNamedObjectA_Call(realBase, (APTR) "RepeatRemove", NULL);

    ok = (BOOL)(newObj && realObj
                && AddNamedObject_Call(newBase, NULL, newObj)
                && AddNamedObject_Call(realBase, NULL, realObj));

    port = CreateMsgPort();
    ok = (BOOL)(ok && port != NULL);

    if (ok) {
        /* --- utility-new.library side --- */
        msg1.mn_ReplyPort = port;
        msg1.mn_Length = sizeof(msg1);
        msg1.mn_Node.ln_Name = NULL;
        RemNamedObject_Call(newBase, newObj, &msg1);
        reply1N = WaitPortTimeout(port, 5);
        ok = (BOOL)(ok && reply1N == &msg1 && reply1N->mn_Node.ln_Name == (char *)newObj);

        /* Object is now fully removed (removed==TRUE, pendingRemoval==NULL,
         * useCount==0). Call RemNamedObject() again with a FRESH message. */
        msg2.mn_ReplyPort = port;
        msg2.mn_Length = sizeof(msg2);
        msg2.mn_Node.ln_Name = (char *)0xDEADBEEF;  /* sentinel: untouched means never replied */
        RemNamedObject_Call(newBase, newObj, &msg2);
        reply2N = WaitPortTimeout(port, 5);

        {
            ULONG diagArgs[2];
            diagArgs[0] = (ULONG)(reply2N != NULL);
            diagArgs[1] = reply2N ? (ULONG)reply2N->mn_Node.ln_Name : 0;
            report((CONST_STRPTR) "DIAG RemNamedObject-Repeat new replied=%lu lnName=%lu\r\n", diagArgs);
        }

        /* --- real ROM-resident utility.library side --- */
        msg1.mn_ReplyPort = port;
        msg1.mn_Length = sizeof(msg1);
        msg1.mn_Node.ln_Name = NULL;
        RemNamedObject_Call(realBase, realObj, &msg1);
        reply1R = WaitPortTimeout(port, 5);
        ok = (BOOL)(ok && reply1R == &msg1 && reply1R->mn_Node.ln_Name == (char *)realObj);

        msg2.mn_ReplyPort = port;
        msg2.mn_Length = sizeof(msg2);
        msg2.mn_Node.ln_Name = (char *)0xDEADBEEF;
        RemNamedObject_Call(realBase, realObj, &msg2);
        reply2R = WaitPortTimeout(port, 5);

        {
            ULONG diagArgs[2];
            diagArgs[0] = (ULONG)(reply2R != NULL);
            diagArgs[1] = reply2R ? (ULONG)reply2R->mn_Node.ln_Name : 0;
            report((CONST_STRPTR) "DIAG RemNamedObject-Repeat real replied=%lu lnName=%lu\r\n", diagArgs);
        }

        /* Real-oracle finding (see provenance/utility-library-namedobject.md's
         * follow-up section): the real ROM DOES reply the second message,
         * but with ln_Name = NULL (the same "removed by another process"
         * signal as the already-pending race case), never the object
         * pointer a second time. Both bases must match this exactly. */
        ok = (BOOL)(ok
                    && reply2N == &msg2 && reply2N->mn_Node.ln_Name == NULL
                    && reply2R == &msg2 && reply2R->mn_Node.ln_Name == NULL);

        DeleteMsgPort(port);
    }

    FreeNamedObject_Call(newBase, newObj);
    FreeNamedObject_Call(realBase, realObj);

    result((CONST_STRPTR) "RemNamedObject-RepeatOnAlreadyRemoved-MatchesOracle", ok);
}

/* ========================================================================
 * Concurrency hardening follow-up (2026-10-13): REAL multi-task races
 * against namedobject.c's new locking (provenance/
 * utility-library-namedobject.md's "Locking and concurrency" section).
 * Everything above this point in the file is single-task/sequential --
 * the two sub-tests below spin up a SECOND, genuinely preemptively
 * scheduled Exec Task (CreateTask(), priority 0 -- the same priority
 * this CLI process itself runs at, so Exec's round-robin time-slicing
 * between equal-priority ready tasks actually interleaves the two
 * rather than one running to completion before the other gets a turn)
 * that calls into the SAME chosen library base concurrently with this,
 * the main, task. Each sub-test is run once entirely against
 * utility-new.library and once entirely against the real ROM-resident
 * utility.library -- same dual-base discipline as every other test in
 * this file, just sequential between the two bases rather than
 * interleaved (the actual concurrency under test is between the TWO
 * TASKS on one base at a time, not between the two bases).
 *
 * CreateTask()'s entry function starts with no parameters -- there is
 * no argument-passing mechanism -- so all shared state between the main
 * task and the spawned task is carried via file-scope statics, same
 * "flat, unprotected address space" assumption this file already
 * documents for strEqualLocal() above.
 * ======================================================================== */

#define RACE_POOL_PER_TASK 15

static struct Library *g_raceBase;
static struct NamedObject *g_raceNS;
static struct NamedObject *g_racePoolB[RACE_POOL_PER_TASK];  /* owned solely by the spawned task */
static LONG g_racePoolBOk;
static struct MsgPort *g_raceDonePort;
static struct Message g_raceDoneMsg;

/* Sub-test 1's spawned-task body: Add then immediately AttemptRem each
 * of ITS OWN RACE_POOL_PER_TASK objects into/out of the SAME shared
 * namespace the main task is concurrently doing the exact same thing to
 * with a DISJOINT set of objects. Neither task ever touches the OTHER
 * task's objects' own state -- this sub-test is purely about whether
 * the shared namespace List's Enqueue()/Remove() linkage survives two
 * tasks mutating it at once, not about a per-object use-count race
 * (that's sub-test 2, below). */
static void ListRaceSecondaryEntry(void)
{
    LONG i, ok = 0;

    for (i = 0; i < RACE_POOL_PER_TASK; i++) {
        if (AddNamedObject_Call(g_raceBase, g_raceNS, g_racePoolB[i])
            && AttemptRemNamedObject_Call(g_raceBase, g_racePoolB[i]))
            ok++;
    }

    g_racePoolBOk = ok;

    g_raceDoneMsg.mn_Node.ln_Type = NT_MESSAGE;
    PutMsg(g_raceDonePort, &g_raceDoneMsg);

    /* REAL-RIG FINDING: a CreateTask() entry function must not simply
     * fall off its own end -- CreateTask() sets up the new task's stack
     * with no return address for an ordinary RTS to find, so an
     * implicit "return" here is undefined behaviour. The first real-rig
     * recording of this test's own two sub-tests back-to-back exposed
     * this directly: the FIRST sub-test's run (against utility-new.
     * library) appeared to pass fine (its own spawned task's undefined
     * post-return behaviour happened not to crash outright), but the
     * SECOND sub-test's run (against the real ROM) then saw its own
     * CreateTask() call fail outright (confirmed via a temporary DIAG
     * probe: both race counters read back 0, meaning the `if (child)`
     * guard around the whole race loop was never entered at all) --
     * consistent with the first spawned task never having been cleanly
     * torn down, leaving the task list/scheduler in a state that broke
     * the next CreateTask() call. RemTask(NULL) (“remove the task
     * that's calling me”) is Exec's own documented way for a task to
     * terminate itself; it never returns to its caller. */
    RemTask(NULL);
}

static void test_concurrent_add_attemptrem_list_integrity(struct Library *base,
                                                            CONST_STRPTR testName)
{
    struct NamedObject *poolA[RACE_POOL_PER_TASK];
    struct NamedObject *ns = NULL;
    struct Task *child;
    LONG i, okA = 0;
    BOOL ok;
    char nameBuf[32];
    /* REAL-RIG FINDING: an object used as someone's nameSpace argument
     * must actually have been allocated WITH ANO_NameSpace=TRUE on the
     * real ROM -- this project's own design decision 1 (namedobject.c's
     * own header comment) permits using ANY object as a namespace
     * unconditionally, an inference the original work package's own
     * provenance flagged as NOT independently oracle-confirmed (see
     * "Known-untested edge cases"). This sub-test's first real-rig
     * recording used a plain, flagless AllocNamedObjectA_Call(base,
     * "RaceNS", NULL) for `ns` and saw EVERY SINGLE AddNamedObject() call
     * against the real ROM fail (okA=0, okB=0 -- diagnosed via a
     * temporary DIAG probe) while the identical shape against
     * utility-new.library succeeded (okA=100, okB=100) -- exactly the
     * untested edge case resolving in the OPPOSITE direction from this
     * project's own inference. Fixed here (not in namedobject.c itself,
     * which is out of this task's scope) by giving `ns` ANO_NameSpace
     * explicitly, matching every other test in this file that uses an
     * object as a namespace. */
    struct TagItem nsTag[] = { { ANO_NameSpace, TRUE }, { TAG_DONE, 0 } };

    for (i = 0; i < RACE_POOL_PER_TASK; i++) {
        poolA[i] = NULL;
        g_racePoolB[i] = NULL;
    }

    ns = (struct NamedObject *)AllocNamedObjectA_Call(base, (APTR) "RaceNS", nsTag);
    ok = (BOOL)(ns != NULL);

    for (i = 0; i < RACE_POOL_PER_TASK && ok; i++) {
        /* Unique name per slot, built directly into a small reusable
         * buffer -- AllocNamedObjectA() copies the name itself (see its
         * own StrCopyLocal() call), so the buffer needn't outlive this
         * call, and reusing it across iterations is safe. */
        ULONG n = (ULONG)i;
        STRPTR p = (STRPTR)(nameBuf + sizeof(nameBuf) - 1);
        *p = 0;
        *--p = 'A';
        do { *--p = (char)('0' + (n % 10)); n /= 10; } while (n);
        poolA[i] = (struct NamedObject *)AllocNamedObjectA_Call(base, (APTR) p, NULL);
        if (!poolA[i])
            ok = FALSE;
    }
    for (i = 0; i < RACE_POOL_PER_TASK && ok; i++) {
        ULONG n = (ULONG)i;
        STRPTR p = (STRPTR)(nameBuf + sizeof(nameBuf) - 1);
        *p = 0;
        *--p = 'B';
        do { *--p = (char)('0' + (n % 10)); n /= 10; } while (n);
        g_racePoolB[i] = (struct NamedObject *)AllocNamedObjectA_Call(base, (APTR) p, NULL);
        if (!g_racePoolB[i])
            ok = FALSE;
    }

    g_raceBase = base;
    g_raceNS = ns;
    g_racePoolBOk = -1;
    g_raceDonePort = CreateMsgPort();
    ok = (BOOL)(ok && g_raceDonePort != NULL);

    if (ok) {
        child = CreateTask((STRPTR) "utilitycheck-race1", 0,
                            (APTR) ListRaceSecondaryEntry, 8192);
        ok = (BOOL)(child != NULL);
    } else {
        child = NULL;
    }

    if (ok) {
        for (i = 0; i < RACE_POOL_PER_TASK; i++) {
            if (AddNamedObject_Call(base, ns, poolA[i])
                && AttemptRemNamedObject_Call(base, poolA[i]))
                okA++;
        }

        ok = (BOOL)(WaitPortTimeout(g_raceDonePort, 30) == &g_raceDoneMsg);
        ok = (BOOL)(ok && okA == RACE_POOL_PER_TASK && g_racePoolBOk == RACE_POOL_PER_TASK);
    }

    /* Both pools were Add()ed then immediately AttemptRem()'d -- the
     * shared namespace should be completely empty again. Walk it to
     * confirm the list is well-formed (terminates) AND genuinely empty:
     * a corrupted Enqueue()/Remove() interleaving could leave a stale
     * node still linked, create a cycle that never reaches the tail
     * sentinel, or simply crash well before this point ever runs. */
    if (ok) {
        struct NamedObject *walk = NULL;
        LONG count = 0;

        for (;;) {
            struct NamedObject *next =
                (struct NamedObject *)FindNamedObject_Call(base, ns, NULL, walk);
            if (!next)
                break;
            ReleaseNamedObject_Call(base, next);  /* FindNamedObject() takes a use count */
            walk = next;
            count++;
            if (count > RACE_POOL_PER_TASK * 2) {
                ok = FALSE;  /* only reachable via a corrupted/cyclic list */
                break;
            }
        }
        ok = (BOOL)(ok && count == 0);
    }

    {
        ULONG diagArgs[2];
        diagArgs[0] = (ULONG)okA;
        diagArgs[1] = (ULONG)g_racePoolBOk;
        report((CONST_STRPTR) "DIAG ConcurrentAddAttemptRem-ListIntegrity okA=%lu okB=%lu\r\n",
               diagArgs);
    }

    if (g_raceDonePort)
        DeleteMsgPort(g_raceDonePort);
    report((CONST_STRPTR) "DIAG race1-cleanup deletedport\r\n", NULL);
    for (i = 0; i < RACE_POOL_PER_TASK; i++) {
        ULONG diagI[1];
        diagI[0] = (ULONG)i;
        report((CONST_STRPTR) "DIAG race1-cleanup freeIter i=%lu\r\n", diagI);
        if (poolA[i])
            FreeNamedObject_Call(base, poolA[i]);
        report((CONST_STRPTR) "DIAG race1-cleanup freedA\r\n", NULL);
        if (g_racePoolB[i])
            FreeNamedObject_Call(base, g_racePoolB[i]);
        report((CONST_STRPTR) "DIAG race1-cleanup freedB\r\n", NULL);
    }
    report((CONST_STRPTR) "DIAG race1-cleanup freedpools\r\n", NULL);
    if (ns)
        FreeNamedObject_Call(base, ns);
    report((CONST_STRPTR) "DIAG race1-cleanup freedns\r\n", NULL);

    result(testName, ok);
}

/* Sub-test 2's spawned-task body: repeatedly FindNamedObject()+
 * ReleaseNamedObject() the ONE shared object both tasks are racing,
 * while the main task does the same AND, partway through, issues the
 * one RemNamedObject() call -- directly exercising the documented "one
 * task holds a reference while another tries to remove the object"
 * scenario this facility's own async handshake exists for (task brief's
 * own suggested shape). */
static LONG g_raceObjIters = 60;
static LONG g_raceObjSecOk;
static LONG g_raceObjSecFail;

static void FindReleaseSecondaryEntry(void)
{
    LONG i, ok = 0, fail = 0;

    for (i = 0; i < g_raceObjIters; i++) {
        struct NamedObject *found = (struct NamedObject *)
            FindNamedObject_Call(g_raceBase, g_raceNS, (APTR) "RaceObj", NULL);
        if (found) {
            ReleaseNamedObject_Call(g_raceBase, found);
            ok++;
        } else {
            fail++;
        }
    }

    g_raceObjSecOk = ok;
    g_raceObjSecFail = fail;

    g_raceDoneMsg.mn_Node.ln_Type = NT_MESSAGE;
    PutMsg(g_raceDonePort, &g_raceDoneMsg);

    /* REAL-RIG FINDING: a CreateTask() entry function must not simply
     * fall off its own end -- CreateTask() sets up the new task's stack
     * with no return address for an ordinary RTS to find, so an
     * implicit "return" here is undefined behaviour. The first real-rig
     * recording of this test's own two sub-tests back-to-back exposed
     * this directly: the FIRST sub-test's run (against utility-new.
     * library) appeared to pass fine (its own spawned task's undefined
     * post-return behaviour happened not to crash outright), but the
     * SECOND sub-test's run (against the real ROM) then saw its own
     * CreateTask() call fail outright (confirmed via a temporary DIAG
     * probe: both race counters read back 0, meaning the `if (child)`
     * guard around the whole race loop was never entered at all) --
     * consistent with the first spawned task never having been cleanly
     * torn down, leaving the task list/scheduler in a state that broke
     * the next CreateTask() call. RemTask(NULL) (“remove the task
     * that's calling me”) is Exec's own documented way for a task to
     * terminate itself; it never returns to its caller. */
    RemTask(NULL);
}

static void test_concurrent_find_release_vs_rem(struct Library *base, CONST_STRPTR testName)
{
    struct NamedObject *ns = NULL, *obj = NULL;
    struct Task *child;
    struct Message msg;
    struct MsgPort *remPort = NULL;
    struct Message *reply;
    LONG i, mainOk = 0, mainFail = 0;
    BOOL ok;
    /* Same real-rig finding as test_concurrent_add_attemptrem_list_
     * integrity above -- `ns` needs ANO_NameSpace=TRUE for the real ROM
     * to actually treat it as a namespace. */
    struct TagItem nsTag[] = { { ANO_NameSpace, TRUE }, { TAG_DONE, 0 } };

    ns = (struct NamedObject *)AllocNamedObjectA_Call(base, (APTR) "RaceObjNS", nsTag);
    obj = (struct NamedObject *)AllocNamedObjectA_Call(base, (APTR) "RaceObj", NULL);
    ok = (BOOL)(ns && obj && AddNamedObject_Call(base, ns, obj));

    remPort = CreateMsgPort();
    ok = (BOOL)(ok && remPort != NULL);

    g_raceBase = base;
    g_raceNS = ns;
    g_raceObjSecOk = -1;
    g_raceObjSecFail = -1;
    g_raceDonePort = CreateMsgPort();
    ok = (BOOL)(ok && g_raceDonePort != NULL);

    if (ok) {
        child = CreateTask((STRPTR) "utilitycheck-race2", 0,
                            (APTR) FindReleaseSecondaryEntry, 8192);
        ok = (BOOL)(child != NULL);
    } else {
        child = NULL;
    }

    if (ok) {
        /* Race the SAME Find+Release pattern against the spawned task
         * for the first half of this task's own iterations, THEN issue
         * the one RemNamedObject() call, THEN keep racing for the
         * second half -- maximizing the chance the real removal lands
         * while the spawned task still holds (or is about to take) its
         * own concurrent reference. */
        for (i = 0; i < g_raceObjIters / 2; i++) {
            struct NamedObject *found = (struct NamedObject *)
                FindNamedObject_Call(base, ns, (APTR) "RaceObj", NULL);
            if (found) {
                ReleaseNamedObject_Call(base, found);
                mainOk++;
            } else {
                mainFail++;
            }
        }

        msg.mn_ReplyPort = remPort;
        msg.mn_Length = sizeof(msg);
        msg.mn_Node.ln_Name = (char *)0xDEADBEEF;  /* sentinel: untouched means never replied */
        RemNamedObject_Call(base, obj, &msg);

        for (; i < g_raceObjIters; i++) {
            struct NamedObject *found = (struct NamedObject *)
                FindNamedObject_Call(base, ns, (APTR) "RaceObj", NULL);
            if (found) {
                ReleaseNamedObject_Call(base, found);
                mainOk++;
            } else {
                mainFail++;
            }
        }

        reply = WaitPortTimeout(remPort, 30);
        ok = (BOOL)(ok && reply == &msg && reply->mn_Node.ln_Name == (char *)obj);

        ok = (BOOL)(ok && WaitPortTimeout(g_raceDonePort, 30) == &g_raceDoneMsg);
    }

    /* Post-conditions, probed only through the public API (same style
     * as every other test in this file -- struct PrivNamedObject's own
     * fields are never reached into directly): the object must now be
     * fully, durably removed from BOTH tasks' point of view, regardless
     * of exactly how the two tasks' Find/Release/Rem calls actually
     * interleaved. */
    ok = (BOOL)(ok
                && AttemptRemNamedObject_Call(base, obj) == 0
                && FindNamedObject_Call(base, ns, (APTR) "RaceObj", NULL) == 0);

    {
        ULONG diagArgs[4];
        diagArgs[0] = (ULONG)mainOk;
        diagArgs[1] = (ULONG)mainFail;
        diagArgs[2] = (ULONG)g_raceObjSecOk;
        diagArgs[3] = (ULONG)g_raceObjSecFail;
        report((CONST_STRPTR)
               "DIAG ConcurrentFindRelease mainOk=%lu mainFail=%lu secOk=%lu secFail=%lu\r\n",
               diagArgs);
    }

    if (remPort)
        DeleteMsgPort(remPort);
    if (g_raceDonePort)
        DeleteMsgPort(g_raceDonePort);
    report((CONST_STRPTR) "DIAG race2-cleanup deletedports\r\n", NULL);
    if (obj)
        FreeNamedObject_Call(base, obj);
    report((CONST_STRPTR) "DIAG race2-cleanup freedobj\r\n", NULL);
    if (ns)
        FreeNamedObject_Call(base, ns);
    report((CONST_STRPTR) "DIAG race2-cleanup freedns\r\n", NULL);

    result(testName, ok);
}

static void test_getuniqueid_twice(struct Library *newBase, struct Library *realBase)
{
    ULONG n1, n2, r1, r2;
    BOOL ok;

    n1 = GetUniqueID_Call(newBase);
    n2 = GetUniqueID_Call(newBase);
    r1 = GetUniqueID_Call(realBase);
    r2 = GetUniqueID_Call(realBase);

    ok = (BOOL)(n1 != n2 && r1 != r2);

    result((CONST_STRPTR) "GetUniqueID-TwoCallsDifferentValues", ok);
}

/* Tenth work package (correction, see provenance/utility-library-tags.md's
 * dated correction section): differential test for the lib_Version/
 * lib_Revision fix. Everything above this point opens both libraries
 * with OpenLibrary(name, 0) -- trivially satisfied by exec.doc's own
 * ">= requested version" rule regardless of what lib_Version actually
 * holds, which is exactly why this bug was invisible to every one of
 * this project's first nine work packages. These two checks are new:
 * one exercises OpenLibrary() itself with a NONZERO version argument
 * (the ordinary way real client code opens a library -- e.g.
 * OpenLibrary("utility.library", 36)), the other reads lib_Version/
 * lib_Revision directly off the already-open base to confirm they hold
 * the exact values the build declares (the CMake project version; the
 * build passes them in, the fallback below matches the current version). */
#ifndef EXPECT_LIB_VERSION
#define EXPECT_LIB_VERSION  40
#define EXPECT_LIB_REVISION 1
#endif

static void test_lib_version_fields(struct Library *newBase)
{
    BOOL ok = (BOOL)(newBase->lib_Version == EXPECT_LIB_VERSION &&
                      newBase->lib_Revision == EXPECT_LIB_REVISION);

    result((CONST_STRPTR) "LibBase-VersionRevisionFieldsMatchDefines", ok);
}

static void test_openlibrary_version_gate(void)
{
    struct Library *lib;

    /* Should succeed: nonzero version at or below our real lib_Version
     * -- the ordinary way real client code opens a library, and
     * the exact case that silently returned NULL before this work
     * package's fix (lib_Version sat at whatever MakeLibrary() left it,
     * confirmed 0, so no N>=1 request could ever succeed). */
    lib = OpenLibrary((CONST_STRPTR) "utility-new.library", EXPECT_LIB_VERSION);
    result((CONST_STRPTR) "OpenLibrary-NonzeroVersionAtOrBelowLibVersion-Succeeds",
           lib != NULL);
    if (lib)
        CloseLibrary(lib);

    /* Should fail: version strictly greater than our real lib_Version
     * -- confirms the gate is a genuine ">=" comparison against the
     * real stored value, not just "accept anything nonzero now that
     * SOME nonzero field is set". */
    lib = OpenLibrary((CONST_STRPTR) "utility-new.library", EXPECT_LIB_VERSION + 1);
    result((CONST_STRPTR) "OpenLibrary-VersionAboveLibVersion-Fails",
           lib == NULL);
    if (lib)
        CloseLibrary(lib);

    /* Baseline, unaffected by this fix: version 0 always succeeds
     * (trivially satisfies ">= requested version") -- must still work
     * identically after the fix, confirming no regression to the
     * existing open-with-version-0 path every other test in this file
     * depends on. */
    lib = OpenLibrary((CONST_STRPTR) "utility-new.library", 0);
    result((CONST_STRPTR) "OpenLibrary-ZeroVersion-StillSucceeds",
           lib != NULL);
    if (lib)
        CloseLibrary(lib);
}

int main(void)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    struct Library *newBase, *realBase;
    ULONG args[2];

    /* Suppress the "insert disk" system requester OpenLibrary() would
     * otherwise pop if utility-new.library isn't found at all (e.g. a
     * misconfigured LIBS: assign) -- same technique as libcheck.c. */
    me->pr_WindowPtr = (APTR)-1L;

    newBase = OpenLibrary((CONST_STRPTR) "utility-new.library", 0);
    args[0] = (ULONG)(APTR)"utility-new.library";
    report((CONST_STRPTR) (newBase ? "OPEN %s\r\n" : "FAIL-OPEN %s\r\n"), args);

    realBase = OpenLibrary((CONST_STRPTR) "utility.library", 0);
    args[0] = (ULONG)(APTR)"utility.library";
    report((CONST_STRPTR) (realBase ? "OPEN %s\r\n" : "FAIL-OPEN %s\r\n"), args);

    if (newBase && realBase) {
        /* Tenth work package's two new checks run FIRST, deliberately,
         * not appended at the end with every other test added by each
         * work package so far: this golden test's own 900-second
         * capture window has never actually been long enough to reach
         * a trailing "DONE pass=.. fail=.." report line (confirmed by
         * inspecting the committed expected.bin from every prior work
         * package -- every one of them cuts off mid-run, inside the
         * V39 NamedObject family's own message-port-based tests near
         * the end of this list, never reaching the end of main()) --
         * this project has apparently always accepted that partial,
         * but still fully deterministic, capture as the fixture.
         * Anything appended after that existing cutoff point would
         * never appear in expected.bin at all, so these two run right
         * after the two OPEN lines instead, guaranteed to land inside
         * the captured window regardless of how long everything after
         * them takes. */
        test_lib_version_fields(newBase);
        test_openlibrary_version_gate();

        test_next_tag_item(newBase, realBase);
        test_find_tag_item(newBase, realBase);
        test_get_tag_data(newBase, realBase);
        test_tag_in_array(newBase, realBase);
        test_allocate_free_round_trip(newBase, realBase);
        test_clone_tag_items(newBase, realBase);
        test_clone_tag_items_null(newBase, realBase);
        test_refresh_tag_item_clones(newBase, realBase);
        test_pack_bool_tags(newBase, realBase);
        test_filter_tag_changes(newBase, realBase, FALSE);
        test_filter_tag_changes(newBase, realBase, TRUE);
        test_map_tags(newBase, realBase, MAP_REMOVE_NOT_FOUND);
        test_map_tags(newBase, realBase, MAP_KEEP_NOT_FOUND);
        test_filter_tag_items(newBase, realBase, TAGFILTER_AND);
        test_filter_tag_items(newBase, realBase, TAGFILTER_NOT);
        test_call_hook_pkt(newBase, realBase);
        test_amiga2date_epoch(newBase, realBase);
        test_date_leap_feb29(newBase, realBase);
        test_date_nonleap_boundary(newBase, realBase);
        test_checkdate_wday_not_checked(newBase, realBase);
        test_checkdate_invalid_mday(newBase, realBase);
        test_smult32(newBase, realBase);
        test_umult32(newBase, realBase);
        test_sdivmod32(newBase, realBase);
        test_udivmod32(newBase, realBase);
        test_smult64_large_negative(newBase, realBase);
        test_smult64_small_negative(newBase, realBase);
        test_smult64_both_negative(newBase, realBase);
        test_umult64(newBase, realBase);
        test_toupper_tolower_full_range(newBase, realBase);
        test_stricmp(newBase, realBase);
        test_strnicmp(newBase, realBase);
        test_apply_tag_changes(newBase, realBase);
        test_pack_structure_tags_roundtrip(newBase, realBase);
        test_pack_structure_tags_bit(newBase, realBase);
        test_pack_structure_tags_exists(newBase, realBase);
        test_unpack_structure_tags_roundtrip(newBase, realBase);
        test_unpack_structure_tags_bit(newBase, realBase);
        test_alloc_free_roundtrip(newBase, realBase);
        test_alloc_userspace(newBase, realBase);
        test_namedobjectname_null(newBase, realBase);
        test_add_find_namespace_and_findall(newBase, realBase);
        test_add_root_namespace(newBase, realBase);
        test_nodups_rejected(newBase, realBase);
        test_attemptrem_never_added_fails(newBase, realBase);
        test_attemptrem_success_after_add_and_release(newBase, realBase);
        test_attemptrem_blocked_while_found(newBase, realBase);
        test_remnamedobject_repeat_on_already_removed(newBase, realBase);
        test_concurrent_add_attemptrem_list_integrity(
            newBase, (CONST_STRPTR) "ConcurrentAddAttemptRem-ListIntegrity-NewLibrary");
        test_concurrent_add_attemptrem_list_integrity(
            realBase, (CONST_STRPTR) "ConcurrentAddAttemptRem-ListIntegrity-RealROM");
        test_concurrent_find_release_vs_rem(
            newBase, (CONST_STRPTR) "ConcurrentFindReleaseVsRemNamedObject-NewLibrary");
        test_concurrent_find_release_vs_rem(
            realBase, (CONST_STRPTR) "ConcurrentFindReleaseVsRemNamedObject-RealROM");
        test_addnamedobject_nonnamespace_object(newBase, realBase);
        test_findnamedobject_nonnamespace_object(newBase, realBase);
        test_getuniqueid_twice(newBase, realBase);
    } else {
        report((CONST_STRPTR) "ABORT cannot run tests, a library failed to open\r\n", NULL);
    }

    args[0] = g_pass;
    args[1] = g_fail;
    report((CONST_STRPTR) "DONE pass=%lu fail=%lu\r\n", args);

    if (realBase)
        CloseLibrary(realBase);
    if (newBase)
        CloseLibrary(newBase);

    return (g_fail == 0 && newBase && realBase) ? 0 : 10;
}
