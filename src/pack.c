/* SPDX-License-Identifier: MPL-2.0 */
// PackStructureTags and UnpackStructureTags.

#include "internal/pack.h"
#include "internal/tags.h"

#include <utility/tagitem.h>
#include <utility/pack.h>

/* ========================================================================
 * PackStructureTags / UnpackStructureTags -- transfer values between a
 * tag list and a plain C structure via a compact bytecode "pack table".
 * (both V39)
 *
 * Autodoc FUNCTION text (utility.doc, quoted in full in the task brief
 * and re-verified directly against the live ndk32-autodocs skill for
 * this work package):
 *   PackStructureTags: "For each table entry, a FindTagItem() will be
 *   done and if the matching tag is found in the taglist, the data
 *   field will be packed into the given structure based on the
 *   packtable definition."
 *   UnpackStructureTags: "For each table entry, a FindTagItem() will be
 *   done and if the matching tag is found in the taglist, the data in
 *   the structure will be placed into the memory pointed to by the
 *   tag's ti_Data. ti_Data *must* point to a LONGWORD."
 *
 * The crucial, easy-to-get-backwards point confirmed directly from that
 * text (also flagged explicitly in the task brief): the PACK TABLE is
 * the outer loop, not the tag list -- for each table entry, FindTagItem()
 * searches tagList for that one specific tag. PackStructureTags copies
 * FROM a found tag's ti_Data INTO the struct; UnpackStructureTags copies
 * FROM the struct's field INTO the LONGWORD ti_Data points to (NOT into
 * ti_Data itself -- ti_Data is a pointer-to-output here, a different
 * convention from every other tag-list function in this file).
 *
 * Pack-table bytecode layout, derived from <utility/pack.h>'s own header
 * comment and cross-checked bit-for-bit against that header's own
 * PKCTRL_ / PSTF_ constant VALUES (not just their names -- see
 * provenance/utility-library-packtags.md for the full derivation
 * showing why e.g. PKCTRL_WORD/PKCTRL_LONG/PKCTRL_BIT's actual bit
 * patterns, not just the header's prose, pin down which two bits are
 * "type" and in which order):
 *
 *   bit 31      : PSTF_SIGNED -- Byte/Word/Long: sign-extend on unpack.
 *                 Bit: "inverted boolean" (invert the packed/unpacked
 *                 sense).
 *   bits 30-29  : direction control -- 00 = pack+unpack (both), 10 =
 *                 pack-only (PKCTRL_PACKONLY sets ONLY bit 30), 01 =
 *                 unpack-only (PKCTRL_UNPACKONLY sets ONLY bit 29), 11 =
 *                 "special" per <utility/pack.h>'s own diagram comment --
 *                 never emitted by any PACK_*() macro in the header and
 *                 not otherwise documented anywhere in utility.doc; not
 *                 implemented here (treated as a no-op/skip -- see
 *                 provenance's "known-untested" section).
 *   bits 28-27  : type -- 00 = Byte, 01 = Word, 10 = Long, 11 = Bit.
 *                 Confirmed from the constants themselves: PKCTRL_UBYTE
 *                 sets neither bit, PKCTRL_UWORD sets bit 27 only,
 *                 PKCTRL_ULONG sets bit 28 only, PKCTRL_BIT sets both.
 *   bit 26      : PSTF_EXISTS -- Bit-type only, "tag exists" hack (see
 *                 below).
 *   bits 25-16  : tag offset from the current base tag (10 bits) --
 *                 tag = baseTag + this field.
 *   bits 15-13  : bit number within the addressed byte (Bit-type only,
 *                 3 bits -- PACK_WORDBIT()/PACK_LONGBIT() already fold
 *                 the word/long field's own byte selection into the
 *                 13-bit struct offset below, via PK_WORDOFFSET()/
 *                 PK_LONGOFFSET(), so a Bit-type entry's struct offset
 *                 always addresses a single BYTE directly -- no
 *                 separate word/long-width handling is needed here).
 *   bits 12-0   : byte offset into the `pack` structure (13 bits).
 *
 * Table walking: packTable[0] is the initial base tag. Each subsequent
 * longword is either: 0 (PACK_ENDTABLE -- stop), -1 (PACK_NEWOFFSET --
 * the FOLLOWING longword is a new base tag, consumed as a pair), or an
 * ordinary field-control entry as decoded above.
 *
 * The PSTF_EXISTS "hack" -- the least obvious part of this spec, called
 * out explicitly in the task brief, and NOT settled by the autodoc/
 * header text alone: the header's own diagram comment says only "For
 * bit operations: 1 = TAG_EXISTS is TRUE". Real-oracle testing (see
 * provenance/utility-library-packtags.md's "Errors and fixes") confirmed
 * this reads correctly for PACK's own direction -- "ignore ti_Data's
 * actual value entirely; the bit packed into the struct is simply TRUE
 * because a matching tag was found at all" -- matching this project's
 * first-draft guess exactly, PackStructureTags below implements it
 * unchanged from that guess.
 *
 * UNPACK's own direction is NOT symmetric with Pack's, and genuinely
 * surprised this project's first-draft implementation (which DID guess
 * symmetric "always write TRUE" behaviour and was proven wrong by the
 * real oracle): see UnpackStructureTags' own Bit-type handling below
 * for the full empirically-derived rule and the oracle data it came
 * from -- in short, the real V40 ROM only ever writes a Bit-type entry
 * into *ti_Data for ONE combination (PSTF_EXISTS set AND the struct's
 * own bit clear), and even then only ever writes FALSE, never TRUE.
 * ======================================================================== */

#define PKS_DIR(entry)       (((ULONG)(entry) >> 29) & 0x3)
#define PKS_TYPE(entry)      (((ULONG)(entry) >> 27) & 0x3)
#define PKS_TAGOFFSET(entry) (((ULONG)(entry) >> 16) & 0x3FF)
#define PKS_BITNUM(entry)    (((ULONG)(entry) >> 13) & 0x7)
#define PKS_STRUCTOFF(entry) ((ULONG)(entry) & 0x1FFF)

#define PK_DIR_BOTH        0
#define PK_DIR_UNPACKONLY  1
#define PK_DIR_PACKONLY    2
#define PK_DIR_SPECIAL     3  /* deliberately unhandled -- see header comment above */

#define PK_TYPE_BYTE 0
#define PK_TYPE_WORD 1
#define PK_TYPE_LONG 2
#define PK_TYPE_BIT  3

ULONG PackStructureTags(APTR pack, ULONG *packTable, struct TagItem *tagList)
{
    const ULONG *tbl = packTable;
    UBYTE *base = (UBYTE *)pack;
    ULONG baseTag;
    ULONG entry;
    ULONG count = 0;

    if (!pack || !packTable)
        return 0;

    baseTag = *tbl++;

    for (;;) {
        entry = *tbl;

        if (entry == 0)
            break;

        if (entry == (ULONG)-1) {
            tbl++;
            baseTag = *tbl++;
            continue;
        }
        tbl++;

        if (PKS_DIR(entry) == PK_DIR_UNPACKONLY || PKS_DIR(entry) == PK_DIR_SPECIAL)
            continue;

        {
            Tag tag = (Tag)(baseTag + PKS_TAGOFFSET(entry));
            struct TagItem *ti = FindTagItem(tag, tagList);
            UBYTE *field;

            if (!ti)
                continue;

            field = base + PKS_STRUCTOFF(entry);

            switch (PKS_TYPE(entry)) {
                case PK_TYPE_BYTE:
                    *field = (UBYTE)ti->ti_Data;
                    break;

                case PK_TYPE_WORD:
                    *(UWORD *)field = (UWORD)ti->ti_Data;
                    break;

                case PK_TYPE_LONG:
                    *(ULONG *)field = ti->ti_Data;
                    break;

                case PK_TYPE_BIT: {
                    UBYTE mask = (UBYTE)(1U << PKS_BITNUM(entry));
                    BOOL bitVal;

                    if (entry & PSTF_EXISTS)
                        bitVal = TRUE;
                    else
                        bitVal = (ti->ti_Data != 0) ? TRUE : FALSE;

                    if (entry & PSTF_SIGNED)
                        bitVal = bitVal ? FALSE : TRUE;

                    if (bitVal)
                        *field = (UBYTE)(*field | mask);
                    else
                        *field = (UBYTE)(*field & ~mask);
                    break;
                }
            }

            count++;
        }
    }

    return count;
}

ULONG UnpackStructureTags(APTR pack, ULONG *packTable, struct TagItem *tagList)
{
    const ULONG *tbl = packTable;
    UBYTE *base = (UBYTE *)pack;
    ULONG baseTag;
    ULONG entry;
    ULONG count = 0;

    if (!pack || !packTable)
        return 0;

    baseTag = *tbl++;

    for (;;) {
        entry = *tbl;

        if (entry == 0)
            break;

        if (entry == (ULONG)-1) {
            tbl++;
            baseTag = *tbl++;
            continue;
        }
        tbl++;

        if (PKS_DIR(entry) == PK_DIR_PACKONLY || PKS_DIR(entry) == PK_DIR_SPECIAL)
            continue;

        {
            Tag tag = (Tag)(baseTag + PKS_TAGOFFSET(entry));
            struct TagItem *ti = FindTagItem(tag, tagList);
            UBYTE *field;
            ULONG value;

            if (!ti || !ti->ti_Data)
                continue;

            field = base + PKS_STRUCTOFF(entry);

            if (PKS_TYPE(entry) == PK_TYPE_BIT) {
                /* Bit-type UnpackStructureTags -- a genuine, empirically
                 * confirmed real-V40-ROM divergence from the "plain
                 * inverse of Pack" behaviour this project's first-draft
                 * implementation assumed (see provenance/
                 * utility-library-packtags.md's "Errors and fixes" for
                 * the full oracle data and derivation). The real ROM
                 * writes *ti_Data for a Bit-type entry in EXACTLY ONE
                 * combination -- PSTF_EXISTS set AND the addressed
                 * struct bit CLEAR -- and even then only ever writes
                 * FALSE (0), never TRUE. Every other combination (a
                 * plain Bit entry regardless of the struct bit's state,
                 * a PSTF_SIGNED/"inverted" entry regardless of state, or
                 * a PSTF_EXISTS entry whose struct bit is SET) is a
                 * confirmed no-write no-op on the real ROM: FindTagItem()
                 * still succeeds and the entry still counts toward the
                 * returned `num`, but the LONGWORD ti_Data points to is
                 * left completely untouched -- no claim is made here
                 * about why the real ROM behaves this way, only that it
                 * does, reproducibly, for every combination this work
                 * package tested (same "confirmed boundary, not
                 * necessarily root-caused" posture already used
                 * elsewhere in this project, e.g. SMult64's own negation
                 * quirk). */
                UBYTE mask = (UBYTE)(1U << PKS_BITNUM(entry));
                BOOL bitSet = (*field & mask) ? TRUE : FALSE;

                if ((entry & PSTF_EXISTS) && !bitSet)
                    *(ULONG *)(ti->ti_Data) = 0UL;

                count++;
                continue;
            }

            switch (PKS_TYPE(entry)) {
                case PK_TYPE_BYTE:
                    if (entry & PSTF_SIGNED)
                        value = (ULONG)(LONG)(signed char)(*field);
                    else
                        value = (ULONG)(*field);
                    break;

                case PK_TYPE_WORD:
                    if (entry & PSTF_SIGNED)
                        value = (ULONG)(LONG)(signed short)(*(UWORD *)field);
                    else
                        value = (ULONG)(*(UWORD *)field);
                    break;

                case PK_TYPE_LONG:
                default:
                    value = *(ULONG *)field;
                    break;
            }

            *(ULONG *)(ti->ti_Data) = value;
            count++;
        }
    }

    return count;
}
