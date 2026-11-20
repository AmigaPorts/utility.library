/* SPDX-License-Identifier: MPL-2.0 */
// 32-bit and 64-bit multiply/divide. These entries do not need A6 and
// preserve A0/A1; see the entry stubs in entry.c.

#include "internal/math.h"

/* ========================================================================
 * SMult32 / UMult32 / SDivMod32 / UDivMod32 -- utility.library's 32-bit
 * math functions. (V36)
 *
 * Autodoc NOTES (identical text on all four, quoted verbatim -- the
 * empirical claim Step 1 of this work package's task existed to verify,
 * not just trust): "Unlike other Amiga library function calls, the
 * utility.library 32 bit math routines do NOT require A6 to be loaded
 * with a pointer to the library base. A6 can contain anything the
 * application wishes. This is in order to avoid overhead in calling
 * them. In addition, the utility.library math routines preserve all
 * address registers including A0 and A1."
 *
 * This file's own implementations below satisfy the A6-independence
 * half for free -- none of the four touches SysBase or any other
 * library-base-relative state, so they work correctly no matter what
 * (if anything) is in A6 at the call site. The A0/A1-preservation half
 * is enforced by the entry stub, NOT by anything in this file:
 * entry.c's ENTRY_MATH() explicitly saves A0/A1 around the JSR into
 * these plain-stack-ABI C functions and restores them before RTS,
 * because an ordinary compiled C function's A0/A1 are not otherwise
 * guaranteed preserved.
 *
 * SYNOPSIS register assignments (all four, from utility.doc): D0,D1 in
 * -> D0 (single-result functions) or D0:D1 (quotient:remainder) out.
 * SDivMod32/UDivMod32 are implemented to return a 64-bit packed value
 * (quotient in the high 32 bits, remainder in the low 32 bits)
 * specifically because m68k-gcc's own `long long` return convention on
 * this target already places the high 32 bits in D0 and the low 32
 * bits in D1 -- exactly the register assignment the autodoc specifies
 * for quotient:remainder -- so ENTRY_MATH()'s generic trampoline
 * needs no function-specific pack/unpack logic for these two.
 *
 * IMPLEMENTATION NOTE -- deliberately NOT using C's plain `*`/`/`/`%`
 * operators here, found the hard way during this work package's own
 * authoring (see provenance/utility-library-math.md's "Errors and
 * fixes" for the full empirical trail -- both volamos AND the real
 * Kickstart 3.1 oracle hung identically at the first math call until
 * this was understood and fixed): on a 68000 baseline target, a plain
 * 32x32 `*`/`/`/`%` compiles to a call out to a GCC libgcc runtime
 * helper (`___mulsi3`/`___divsi3`/`___udivsi3`/`___umodsi3` -- the 68000
 * has no native 32x32->32 multiply or 32/32 divide instruction). Those
 * helpers work fine in an ordinary crt0-started executable (confirmed:
 * a standalone test program linking tags.c directly and calling
 * SMult32() as a plain C function got the right answer every time) but
 * reproducibly hang BOTH volamos and real Kickstart 3.1 when reached
 * through this project's `-nostartfiles` disk-loaded library path --
 * root cause not fully isolated (plausibly something about the helper
 * expecting runtime state only crt0 normally sets up), but definitively
 * narrowed down to "needs a libgcc runtime-helper call" vs "doesn't",
 * confirmed via a from-scratch byte-level trace of the real jump table,
 * the real entry trampoline, and the real `_SMult32`/`___mulsi3` machine
 * code in the loaded library's own memory (all independently verified
 * byte-correct -- the bug is in actually *reaching* the runtime helper
 * from this build configuration, not in anything this project's own
 * code emits). Sidestepped entirely by implementing both multiply and
 * divide/modulus using only native 68000 instructions GCC can inline
 * directly, with zero external symbol calls: `UMult32`'s three partial
 * products each compile to a single `MULU.W` (16x16->32, a real 68000
 * instruction) via the `mulu16x16()` inline-asm helper below, and
 * `UDivMod32`'s binary long-division loop uses only shifts/compares/
 * subtracts (confirmed via `-S` inspection: zero `jsr`/`bsr` instructions
 * in the compiled output for either). This is also a better match for
 * the autodoc's own stated design intent ("in order to avoid overhead
 * in calling them") than a C-operator-based implementation would be --
 * a lean, self-contained routine, not a thin wrapper around a generic
 * runtime call.
 * ======================================================================== */

/* mulu16x16 -- full 32-bit product of two 16-bit unsigned values via a
 * single native MULU.W instruction. GCC's own `(ULONG)uword_a *
 * (ULONG)uword_b` pattern-matching to MULU.W turned out to be
 * unreliable in practice here (confirmed by -S inspection: it caught
 * exactly one of three structurally-identical multiplies in
 * UMult32Core's school-multiplication expansion and routed the other
 * two through ___mulsi3 anyway), so this is pinned down with explicit
 * inline asm instead of trusted to compiler pattern-matching. */
static ULONG mulu16x16(UWORD a, UWORD b)
{
    ULONG result;
    __asm__ __volatile__ ("mulu.w %1,%0" : "=d"(result) : "d"(b), "0"((ULONG)a));
    return result;
}

/* UMult32Core -- school (long) multiplication via three 16x16->32
 * partial products: result = al*bl + ((ah*bl + al*bh) << 16). Only the
 * low 32 bits of the true 64-bit product are needed (both SMult32 and
 * UMult32 are documented as 32x32->32, truncating), and since two's-
 * complement truncating multiplication is bit-identical for signed and
 * unsigned operands, SMult32 reuses this directly on the bit pattern. */
ULONG UMult32Core(ULONG a, ULONG b)
{
    UWORD al = (UWORD)a, ah = (UWORD)(a >> 16);
    UWORD bl = (UWORD)b, bh = (UWORD)(b >> 16);
    ULONG lo = mulu16x16(al, bl);
    ULONG hi = mulu16x16(ah, bl) + mulu16x16(al, bh);

    return lo + (hi << 16);
}

LONG SMult32(LONG arg1, LONG arg2)
{
    return (LONG)UMult32Core((ULONG)arg1, (ULONG)arg2);
}

ULONG UMult32(ULONG arg1, ULONG arg2)
{
    return UMult32Core(arg1, arg2);
}

/* UDivMod32Core -- unsigned binary long division (32 iterations,
 * shift-compare-subtract), avoiding `/`/`%` entirely -- see this
 * section's own header comment for why. Division by zero is not
 * documented by the autodoc for either function; this implementation's
 * loop leaves quotient as all-1-bits and remainder equal to dividend in
 * that case (every "remainder >= divisor" comparison is trivially true
 * against a zero divisor) -- a defined, non-crashing result, not
 * independently oracle-confirmed since the autodoc gives no documented
 * behaviour to check against. */
unsigned long long UDivMod32Core(ULONG dividend, ULONG divisor)
{
    ULONG quotient = 0, remainder = 0;
    int i;

    for (i = 31; i >= 0; i--) {
        remainder = (remainder << 1) | ((dividend >> i) & 1);
        if (remainder >= divisor) {
            remainder -= divisor;
            quotient |= (1UL << i);
        }
    }

    return (((unsigned long long)quotient) << 32) | remainder;
}

signed long long SDivMod32(LONG dividend, LONG divisor)
{
    /* Unsigned-arithmetic negation (rather than plain unary `-` on a
     * LONG) sidesteps signed-overflow UB for LONG_MIN, whose magnitude
     * isn't representable as a positive LONG. */
    ULONG udividend = (dividend < 0) ? (ULONG)(0UL - (ULONG)dividend) : (ULONG)dividend;
    ULONG udivisor = (divisor < 0) ? (ULONG)(0UL - (ULONG)divisor) : (ULONG)divisor;
    unsigned long long uqr = UDivMod32Core(udividend, udivisor);
    ULONG uquot = (ULONG)(uqr >> 32);
    ULONG urem = (ULONG)uqr;
    /* C truncating-division semantics (matches the direct-C-operator
     * behaviour this replaces): quotient's sign is the XOR of the
     * operands' signs; remainder takes the dividend's sign.
     *
     * Quotient negation uses the same unsigned-arithmetic trick as
     * udividend/udivisor above, not plain unary `-` on a LONG: uquot can
     * legitimately be 0x80000000 (e.g. dividend=LONG_MIN, divisor=1,
     * whose correct quotient is LONG_MIN itself), and negating that via
     * signed `-(LONG)uquot` is the exact same LONG_MIN-overflow UB this
     * function already took care to avoid one step earlier.
     *
     * Remainder negation uses the SAME unsigned-arithmetic trick, for a
     * reason review caught (2026-10-13, see
     * provenance/utility-library-math.md's own follow-up section): the
     * division algorithm only guarantees urem < udivisor <= 0x80000000
     * for a NONZERO divisor. UDivMod32Core's own documented
     * divide-by-zero behaviour (divisor == 0, elsewhere in this file)
     * returns remainder == dividend, so urem CAN reach 0x80000000 when
     * udivisor is 0 (dividend == LONG_MIN, divisor == 0) -- a case a
     * prior version of this comment overlooked when it claimed plain
     * `-(LONG)urem` was "always safe." Using the unsigned-negation trick
     * here too removes the signed-overflow UB unconditionally, the same
     * way it was already removed for the quotient above, rather than
     * merely documenting around it; division-by-zero's overall RESULT
     * remains undefined/unconfirmed against the real ROM either way
     * (unchanged, already disclosed elsewhere in this file). */
    LONG quot = ((dividend < 0) != (divisor < 0)) ? (LONG)(0UL - uquot) : (LONG)uquot;
    LONG rem = (dividend < 0) ? (LONG)(0UL - urem) : (LONG)urem;

    return (((unsigned long long)(ULONG)quot) << 32) | (ULONG)rem;
}

unsigned long long UDivMod32(ULONG dividend, ULONG divisor)
{
    return UDivMod32Core(dividend, divisor);
}

/* ========================================================================
 * SMult64 / UMult64 -- utility.library's 32x32->64 bit multiply
 * functions. (V39)
 *
 * Autodoc NOTES (identical text to SMult32/UMult32/SDivMod32/UDivMod32,
 * quoted verbatim -- re-verified directly against the live
 * ndk32-autodocs skill for this work package, not just carried over by
 * assumption from the third work package's own transcription):
 * "Unlike other Amiga library function calls, the utility.library 32
 * bit math routines do NOT require A6 to be loaded with a pointer to
 * the library base. A6 can contain anything the application wishes.
 * This is in order to avoid overhead in calling them. In addition, the
 * utility.library math routines preserve all address registers
 * including A0 and A1." Same ENTRY_MATH() entry stub (entry.c)
 * enforces the A0/A1-preservation half, exactly as for the 32-bit
 * family -- nothing in this file needs to care about it.
 *
 * SYNOPSIS register assignments, from utility.doc, confirmed to differ
 * from SMult32/UMult32 in exactly the way the task's own framing
 * anticipated -- same D0,D1 INPUT shape, but a genuinely wider,
 * non-truncating OUTPUT:
 *
 *     SMult64: D0,D1 in -> D0:D1 out (64-bit signed product)
 *     UMult64: D0,D1 in -> D0:D1 out (64-bit unsigned product)
 *
 * FUNCTION text confirms the output is the full 64-bit product, NOT the
 * 32-bit truncated product SMult32/UMult32 give: "Returns the signed
 * [unsigned] 64 bit result of multiplying arg1 by arg2." (SMult64's and
 * UMult64's own FUNCTION sections, respectively.)
 *
 * NOTE on the autodoc's own C-prototype text: utility.doc's SYNOPSIS
 * literally shows "LONG SMult64(LONG,LONG);" / "ULONG UMult64(ULONG,
 * ULONG);" -- a single-register notional prototype that does NOT match
 * the D0:D1 output shown two lines above it in the same SYNOPSIS block.
 * This project's own SDivMod32/UDivMod32 (third work package) already
 * established the house convention of trusting the register-assignment
 * line over this kind of single-register notional C prototype, and
 * declaring the actual C implementation with a `long long` return type
 * that matches m68k-gcc's own D0:D1 (high:low) return convention
 * instead -- applied identically here for consistency, not a new
 * judgement call.
 *
 * IMPLEMENTATION NOTE -- same "no libgcc helper calls reachable from
 * this project's -nostartfiles library build" discipline as SMult32/
 * UMult32/SDivMod32/UDivMod32 above (see their own header comment for
 * the full empirical trail of why this matters here, not just in
 * theory). `umul64()` below is the natural extension of UMult32Core()'s
 * three-partial-product schoolbook multiplication to a full, un-
 * truncated 64-bit product -- four 16x16->32 partial products via the
 * same `mulu16x16()` helper, combined with explicit 32-bit overflow/
 * carry detection (the `(sum < operand)` idiom) rather than any 64-bit
 * multiply/shift/subtract operator that might risk inviting a wider
 * libgcc helper in through the back door. The only 64-bit C operation
 * used anywhere in this implementation is the final hi:lo pack into the
 * return value, exactly the same idiom UDivMod32Core() already uses and
 * already confirmed call-free. Confirmed via `-S` inspection, same as
 * every prior math-adjacent work package: zero `jsr`/`bsr` instructions
 * in the compiled output for either function (see provenance/
 * utility-library-math64.md).
 *
 * ************************************************************************
 * DOC-VS-ORACLE DIVERGENCE -- SMult64's NEGATIVE-RESULT PATH, CONFIRMED
 * AGAINST REAL KICKSTART 3.1, NOT ASSUMED. Per the project's "the oracle
 * wins over the docs" rule: utility.doc's FUNCTION text ("Returns the
 * signed 64 bit result of multiplying arg1 by arg2") describes a plain,
 * mathematically exact 64-bit product with no documented exception --
 * but the real V40 ROM's SMult64 does NOT return the mathematically
 * exact value whenever exactly one of arg1/arg2 is negative. Four
 * independent oracle probes (tests/utilitycheck.c's
 * SMult64-ResultAndA0A1Preserved plus three throwaway diagnostic calls
 * during this work package's own authoring, all against the real ROM,
 * not volamos) pinned the exact, reproducible shape of the discrepancy:
 *
 *   SMult64(-70000, 70000): real D0:D1 = 0xFFFFFFFF:0xDBEFEEFF
 *     (mathematically correct value: 0xFFFFFFFE:0xDBEFEF00)
 *   SMult64(70000, -70000): IDENTICAL real D0:D1 (confirms the bug is
 *     symmetric in which operand is negative, as multiplication itself
 *     is)
 *   SMult64(-70000, -70000): real D0:D1 = 0x00000001:0x24101100, i.e.
 *     mathematically exact (+4,900,000,000) -- confirms the bug is
 *     specific to the NEGATIVE-RESULT path; a positive result from two
 *     negative operands is untouched
 *   SMult64(-7, 6): real D0:D1 = 0x00000000:0xFFFFFFD6
 *     (mathematically correct value: 0xFFFFFFFF:0xFFFFFFD6 -- note the
 *     LOW word already matches here, only the high word differs)
 *   SMult64(-100000, 100000): real D0:D1 = 0xFFFFFFFE:0xABF41BFF
 *     (mathematically correct value: 0xFFFFFFFD:0xABF41C00)
 *
 * Deriving the pattern from these four data points (treating `hi0:lo0`
 * as the correctly-computed UNSIGNED product of |arg1| and |arg2| --
 * i.e. exactly what `umul64()` on the absolute values gives): the real
 * ROM's negation, when a negative result is due, behaves as though the
 * two's-complement "+1" carry-in is applied ONLY to the high word
 * (unconditionally, regardless of whether `lo0` was itself zero) and is
 * separately, ALSO applied to the low word only when `hi0` happens to
 * be zero (i.e. when the magnitude product fits entirely in 32 bits).
 * Put as a formula: when arg1 and arg2 have opposite signs,
 *
 *     hi_result = (~hi0 + 1) mod 2^32                         -- ALWAYS
 *     lo_result = (~lo0 + (hi0 == 0 ? 1 : 0)) mod 2^32
 *
 * This reproduces all four oracle data points exactly (verified by
 * hand, byte for byte, including the -7*6 case where only the high word
 * is affected and the -70000/-100000 cases where only the low word is
 * affected) and is symmetric in argument order as required. No claim is
 * made about root cause -- same "confirmed boundary, not necessarily
 * root-caused" posture the third work package already took for
 * UDivMod32/SDivMod32's division-by-zero behaviour -- but the shape is
 * reproducible and deterministic, not noise: this project's job is to
 * replace the real V40 behaviour, bugs included (per the `h`-bit and
 * MapTags precedents already logged elsewhere in this file and in
 * provenance/utility-library-math.md), not the hypothetically "more
 * correct" documented one. `smult64NegateQuirk()` below implements
 * exactly this formula; the three new golden-tested cases in
 * tests/utilitycheck.c (large negative*positive, small
 * negative*positive, both-negative) exercise all three branches
 * (hi0!=0 affected-low-word, hi0==0 affected-high-word-only, and the
 * unaffected positive-result path) against the real oracle.
 * ************************************************************************
 * ======================================================================== */

/* umul64 -- full, untruncated 64-bit product of two 32-bit unsigned
 * values via four 16x16->32 partial products (mulu16x16(), reused
 * unchanged from the 32-bit math functions above) and explicit 32-bit
 * carry propagation using the standard unsigned-overflow idiom
 * (`sum < either_operand` detects a wrapped add) -- no 64-bit multiply,
 * shift, or divide operator is used anywhere in this function.
 *
 * Splitting a = ah:al, b = bh:bl (each UWORD halves), the full product
 * is ah*bh*2^32 + (ah*bl + al*bh)*2^16 + al*bl. `cross` holds
 * (ah*bl + al*bh) mod 2^32, with `crossCarry` recording whether that
 * addition itself overflowed 32 bits (i.e. bit 32 of the true,
 * unwrapped cross value). `cross << 16` (a plain 32-bit shift, native)
 * contributes cross's low 16 bits into lo's bits 16-31 for free -- its
 * own overflow out of lo's top bit is `loCarry`. The high word collects
 * p11 (ah*bh), cross's own top 16 bits (cross >> 16), crossCarry
 * promoted up to bit 16 of the high word, and loCarry. */
static void umul64(ULONG a, ULONG b, ULONG *hiOut, ULONG *loOut)
{
    UWORD al = (UWORD)a, ah = (UWORD)(a >> 16);
    UWORD bl = (UWORD)b, bh = (UWORD)(b >> 16);

    ULONG p00 = mulu16x16(al, bl);
    ULONG p01 = mulu16x16(al, bh);
    ULONG p10 = mulu16x16(ah, bl);
    ULONG p11 = mulu16x16(ah, bh);

    ULONG cross = p01 + p10;
    ULONG crossCarry = (cross < p01) ? 1UL : 0UL;

    ULONG lo = p00 + (cross << 16);
    ULONG loCarry = (lo < p00) ? 1UL : 0UL;

    ULONG hi = p11 + (cross >> 16) + (crossCarry << 16) + loCarry;

    *hiOut = hi;
    *loOut = lo;
}

unsigned long long UMult64(ULONG arg1, ULONG arg2)
{
    ULONG hi, lo;

    umul64(arg1, arg2, &hi, &lo);
    return (((unsigned long long)hi) << 32) | (unsigned long long)lo;
}

/* smult64NegateQuirk -- reproduces the real V40 ROM's own (buggy, but
 * confirmed reproducible) negation shape for SMult64's negative-result
 * path -- see this section's own "DOC-VS-ORACLE DIVERGENCE" header
 * comment above for the full derivation and the four oracle data
 * points it was derived from. `hi0:lo0` is the correctly-computed
 * UNSIGNED product of |arg1| and |arg2|. Deliberately NOT a correct
 * two's-complement negate (a correct one would conditionally carry the
 * low word's own "+1" into the high word only when lo0==0) -- this
 * matches the oracle's own, different, empirically-derived rule
 * instead: the high word always receives the "+1"; the low word only
 * receives it when hi0 is zero. */
static void smult64NegateQuirk(ULONG hi0, ULONG lo0, ULONG *hiOut, ULONG *loOut)
{
    *hiOut = ~hi0 + 1UL;
    *loOut = ~lo0 + ((hi0 == 0) ? 1UL : 0UL);
}

signed long long SMult64(LONG arg1, LONG arg2)
{
    /* Unsigned-arithmetic negation (rather than plain unary `-` on a
     * LONG) sidesteps signed-overflow UB for LONG_MIN, same technique
     * SDivMod32 already established above. */
    ULONG amag = (arg1 < 0) ? (ULONG)(0UL - (ULONG)arg1) : (ULONG)arg1;
    ULONG bmag = (arg2 < 0) ? (ULONG)(0UL - (ULONG)arg2) : (ULONG)arg2;
    ULONG hi0, lo0, hi, lo;

    umul64(amag, bmag, &hi0, &lo0);

    if ((arg1 < 0) != (arg2 < 0)) {
        /* Negative result due -- apply the oracle's own quirky
         * negation shape, not a mathematically clean one. */
        smult64NegateQuirk(hi0, lo0, &hi, &lo);
    } else {
        /* Positive result (both operands non-negative, or both
         * negative) -- the magnitude product IS the answer, confirmed
         * unaffected by any quirk (see the -70000*-70000 oracle data
         * point above). */
        hi = hi0;
        lo = lo0;
    }

    return (signed long long)((((unsigned long long)hi) << 32) | (unsigned long long)lo);
}
