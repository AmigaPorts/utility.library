/* SPDX-License-Identifier: MPL-2.0 */
// ToUpper, ToLower, Stricmp and Strnicmp.

#include "internal/string.h"

/* ========================================================================
 * ToUpper / ToLower -- single-character case conversion, "handling
 * international character sets." (V37)
 *
 * Autodoc FUNCTION text (identical framing on both): "Converts a
 * character to upper [lower] case, handling international character
 * sets." NOTES (identical on all four string/char functions in this
 * section): "Whenever locale.library is installed in a system, this
 * function is replaced by language-specific code. [...] depending on
 * which language the user has currently selected, a given character may
 * return different results when passed to this function."
 *
 * "International character sets" is vague in the autodoc text alone --
 * per the "oracle wins over the docs" rule, the exact byte
 * values converted in the 0x80-0xFF range were determined empirically
 * against the real, un-patched (no locale.library installed) V40 oracle,
 * not assumed from ASCII alone. An exhaustive, full-0x00-0xFF-range
 * differential test (tests/utilitycheck.c's
 * ToUpper-ToLower-FullByteRange case) against the real ROM found exactly
 * one divergence from a first-draft hypothesis of "standard ISO 8859-1
 * case-pairing, skipping the non-letter multiplication/division signs at
 * 0xD7/0xF7": the real V40 ROM does NOT carve out that exception -- it
 * pairs the ENTIRE 0xC0-0xDE <-> 0xE0-0xFE block uniformly, including
 * 0xD7 ('x' multiplication sign) <-> 0xF7 ('/' division sign), even
 * though neither is actually a letter. (0xDF, the German sharp-S with no
 * single-byte uppercase form, and 0xFF, confirmed to have no lowercase
 * pair either, both correctly fall OUTSIDE the paired block and are
 * genuinely identity-mapped -- matching the real ROM exactly, no
 * exception needed there.) Implemented below per the oracle's actual
 * behaviour, not the ISO-8859-1 standard's own letter-only convention --
 * see provenance/utility-library-stringops.md for the full empirical
 * trail (first recording attempt's exact FAIL/DIAG output, the fix, and
 * the clean re-recording).
 * ======================================================================== */
UBYTE ToUpper(UBYTE ch)
{
    if (ch >= 'a' && ch <= 'z')
        return (UBYTE)(ch - 'a' + 'A');

    if (ch >= 0xE0 && ch <= 0xFE)
        return (UBYTE)(ch - 0x20);

    return ch;
}

UBYTE ToLower(UBYTE ch)
{
    if (ch >= 'A' && ch <= 'Z')
        return (UBYTE)(ch - 'A' + 'a');

    if (ch >= 0xC0 && ch <= 0xDE)
        return (UBYTE)(ch + 0x20);

    return ch;
}

/* ========================================================================
 * Stricmp / Strnicmp -- case-insensitive string comparison. (V37)
 *
 * Autodoc FUNCTION text (Stricmp): "This function compares two strings,
 * ignoring case using a generic case conversion routine. If the strings
 * have different lengths, the shorter is treated as if it were extended
 * with zeros." Strnicmp adds: "This function never compares more than
 * 'length' characters."
 *
 * "Shorter string extended with zeros" falls out for free from an
 * ordinary byte-by-byte comparison loop that keeps comparing through the
 * NUL terminator itself (rather than stopping as soon as either string
 * hits its own NUL without comparing that NUL byte against the other
 * string's corresponding byte) -- a NUL terminator IS the value zero, so
 * comparing it against whatever byte the longer string has at that same
 * position is exactly "treating the shorter string as zero-extended."
 * This matters in practice (not just a restatement of ordinary strcmp):
 * an implementation that instead stops comparing as soon as it notices
 * *either* string has ended, treating any two strings sharing an equal-
 * length common prefix as equal regardless of what follows in the
 * longer one, would NOT match this documented behaviour. Both functions
 * below reuse ToUpper() for case-folding, per this file's own reuse
 * discipline.
 *
 * RESULT VALUE -- empirically confirmed against the real oracle, not
 * assumed: the autodoc's own RESULTS text only promises a sign
 * ("<0 means string1 < string2" etc.), and a first-draft implementation
 * here returned the raw byte difference (e.g. 'C'-NUL = 67 for "ABC" vs
 * "AB"), which has the right sign but does NOT match the real ROM's own
 * exact return value (confirmed: the real V40 ROM normalizes its result
 * to exactly -1/0/1, not a raw difference -- this project's own golden-
 * test recording caught this directly, via its DIAG-Stricmp/DIAG-
 * Strnicmp diagnostic lines, not by inspection). Normalized to -1/0/1
 * below to match bit-for-bit.
 * ======================================================================== */
LONG Stricmp(STRPTR string1, STRPTR string2)
{
    UBYTE c1, c2;

    for (;;) {
        c1 = ToUpper(*(UBYTE *)string1);
        c2 = ToUpper(*(UBYTE *)string2);

        if (c1 != c2)
            return (c1 < c2) ? -1 : 1;
        if (c1 == 0)
            return 0;

        string1++;
        string2++;
    }
}

LONG Strnicmp(STRPTR string1, STRPTR string2, LONG length)
{
    UBYTE c1, c2;
    LONG i;

    for (i = 0; i < length; i++) {
        c1 = ToUpper(*(UBYTE *)string1);
        c2 = ToUpper(*(UBYTE *)string2);

        if (c1 != c2)
            return (c1 < c2) ? -1 : 1;
        if (c1 == 0)
            return 0;

        string1++;
        string2++;
    }

    return 0;
}
