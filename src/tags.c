/* SPDX-License-Identifier: MPL-2.0 */
// Tag list functions: iteration, lookup, allocation, filtering and mapping.

#include "internal/library.h"
#include "internal/tags.h"

#include <exec/memory.h>
#include <utility/tagitem.h>

#define __NOLIBBASE__
#include <proto/exec.h>

/* ========================================================================
 * NextTagItem -- iterate through a tag list. (V36)
 *
 * Autodoc FUNCTION text: "Iterates through a tag list, skipping and
 * chaining as dictated by system tags. TAG_SKIP will cause it to skip the
 * entry and a number of following tags as specified in ti_Data. TAG_IGNORE
 * ignores that single entry, and TAG_MORE has a pointer to another array of
 * tags (and terminates the current array!). TAG_DONE also terminates the
 * current array."
 *
 * Autodoc WARNING: "Do NOT use the value of *tagItemPtr, but rather use the
 * pointer returned by NextTagItem()." -- satisfied here because we only
 * ever advance *tagItemPtr ourselves (to one slot past the item we are
 * about to return, or to NULL at end-of-list/TAG_MORE-chain-end); callers
 * who stash *tagItemPtr between calls instead of the return value would see
 * a position mid-chain that doesn't reflect where the next real scan
 * should resume from TAG_MORE's perspective -- hence the warning.
 *
 * FindTagItem/GetTagData below both walk lists exclusively through this
 * function, so all three functions agree by construction on exactly which
 * entries TAG_SKIP/TAG_IGNORE/TAG_MORE hide or reveal.
 * ======================================================================== */
struct TagItem *NextTagItem(struct TagItem **tagItemPtr)
{
    struct TagItem *tag;

    if (!tagItemPtr)
        return NULL;

    tag = *tagItemPtr;

    for (;;) {
        if (!tag) {
            *tagItemPtr = NULL;
            return NULL;
        }

        switch (tag->ti_Tag) {
            case TAG_DONE: /* == TAG_END */
                *tagItemPtr = NULL;
                return NULL;

            case TAG_IGNORE:
                tag++;
                continue;

            case TAG_MORE:
                /* ti_Data points to a continuation array; this also ends
                 * the current array for iteration purposes (autodoc). */
                tag = (struct TagItem *)tag->ti_Data;
                continue;

            case TAG_SKIP:
                /* Skip this entry and the next ti_Data entries. */
                tag += (tag->ti_Data + 1);
                continue;

            default:
                *tagItemPtr = tag + 1;
                return tag;
        }
    }
}

/* ========================================================================
 * FindTagItem -- scan a tag list for a specific tag. (V36)
 *
 * Autodoc: "Scans a tag list and returns a pointer to the first item with
 * ti_Tag matching the 'tagValue' parameter." tagList may be NULL.
 *
 * Implemented strictly on top of NextTagItem() per the task's own
 * discipline (reuse the list-walk, don't re-implement it) so FindTagItem
 * and NextTagItem can never disagree about TAG_SKIP/TAG_IGNORE/TAG_MORE
 * handling.
 * ======================================================================== */
struct TagItem *FindTagItem(Tag tagValue, struct TagItem *tagList)
{
    struct TagItem *tstate = tagList;
    struct TagItem *tag;

    while ((tag = NextTagItem(&tstate)) != NULL) {
        if (tag->ti_Tag == tagValue)
            return tag;
    }

    return NULL;
}

/* ========================================================================
 * GetTagData -- obtain the data corresponding to a tag. (V36)
 *
 * Autodoc: "Searches a tag list for a matching tag, and returns the
 * corresponding ti_Data value for the TagItem found. If no match is found,
 * this function returns the value passed in as 'default'."
 * ======================================================================== */
ULONG GetTagData(Tag tagValue, ULONG defaultVal, struct TagItem *tagList)
{
    struct TagItem *tag = FindTagItem(tagValue, tagList);

    return tag ? tag->ti_Data : defaultVal;
}

/* ========================================================================
 * TagInArray -- check if a tag value appears in an array of tag values.
 * (V36)
 *
 * Autodoc: distinct signature from FindTagItem -- this takes "a simple
 * array of tag values terminated by TAG_DONE", i.e. a plain Tag (ULONG)
 * array, NOT a TagItem list. No TAG_SKIP/TAG_IGNORE/TAG_MORE handling
 * applies here (those are TagItem-list-only concepts).
 * ======================================================================== */
/* LONG, not BOOL: the result must fill all of D0. */
LONG TagInArray(Tag tagValue, Tag *tagArray)
{
    if (!tagArray)
        return FALSE;

    while (*tagArray != TAG_DONE) {
        if (*tagArray == tagValue)
            return TRUE;
        tagArray++;
    }

    return FALSE;
}

/* ========================================================================
 * AllocateTagItems / FreeTagItems / CloneTagItems / RefreshTagItemClones
 *
 * A matched quartet over exec.library's AllocMem()/FreeMem(). FreeTagItems
 * only ever receives the TagItem pointer back (no length), so the
 * allocation size must be recoverable from the pointer alone: we stash it
 * in a hidden ULONG header immediately before the returned array (the same
 * "size just before the pointer" idiom AmigaOS's own AllocVec()/FreeVec()
 * use) and consume it in FreeTagItems(). CloneTagItems() allocates through
 * AllocateTagItems() precisely so its clones can be freed the exact same
 * way as a directly-allocated list, per its autodoc's "This tag list must
 * eventually by freed by calling FreeTagItems()".
 * ======================================================================== */

/* Internal: AllocMem()'d block layout is [ULONG size][TagItem...] --
 * 'size' is the byte length of the TagItem array that follows (NOT
 * including this header), so FreeTagItems() can recover exactly how much
 * to FreeMem(). */
#define TAGHDR_SIZE (sizeof(ULONG))

/* ------------------------------------------------------------------------
 * AllocateTagItems -- allocate a tag list. (V36)
 *
 * Autodoc: "Allocates the specified number of usable TagItems slots. [...]
 * An allocated tag list must eventually be freed using FreeTagItems()."
 * The caller is responsible for how many slots they ask for, including
 * room for their own TAG_DONE terminator if they want one -- the RKRM
 * example allocates exactly as many slots as it fills, with the last one
 * set to TAG_DONE. MEMF_CLEAR means any slot the caller never touches
 * naturally reads back as ti_Tag == 0 == TAG_DONE.
 * ------------------------------------------------------------------------ */
struct TagItem *AllocateTagItems(ULONG numTags, struct UtilityLibrary *base)
{
    struct ExecBase *SysBase = base->exec;
    ULONG size = numTags * sizeof(struct TagItem);
    UBYTE *block;

    block = AllocMem(size + TAGHDR_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
    if (!block)
        return NULL;

    *(ULONG *)block = size;

    return (struct TagItem *)(block + TAGHDR_SIZE);
}

/* ------------------------------------------------------------------------
 * FreeTagItems -- free an allocated tag list. (V36)
 *
 * Autodoc: "Frees the memory of a TagItem list allocated either by
 * AllocateTagItems() or CloneTagItems()." tagList may be NULL (no-op).
 * ------------------------------------------------------------------------ */
VOID FreeTagItems(struct TagItem *tagList, struct UtilityLibrary *base)
{
    struct ExecBase *SysBase = base->exec;
    UBYTE *block;
    ULONG size;

    if (!tagList)
        return;

    block = (UBYTE *)tagList - TAGHDR_SIZE;
    size = *(ULONG *)block;

    FreeMem(block, size + TAGHDR_SIZE);
}

/* ------------------------------------------------------------------------
 * CloneTagItems -- copy a tag list. (V36)
 *
 * Autodoc: "Copies the essential contents of a tag list into a new tag
 * list. The cloning is such that calling FindTagItem() with a given tag on
 * the original or cloned tag lists will always return the same tag value.
 * [...] original - [...] May be NULL, in which case an empty tag list is
 * returned." and "This tag list must eventually by freed by calling
 * FreeTagItems()."
 *
 * Implementation walks 'original' via NextTagItem() (so it transparently
 * flattens any TAG_SKIP/TAG_IGNORE/TAG_MORE chaining in the source into a
 * single contiguous, TAG_DONE-terminated array), counting first so the
 * destination can be sized exactly, then copying. The clone is allocated
 * through AllocateTagItems() so it can later be freed/refreshed by the
 * ordinary FreeTagItems()/RefreshTagItemClones() machinery.
 * ------------------------------------------------------------------------ */
struct TagItem *CloneTagItems(struct TagItem *original, struct UtilityLibrary *base)
{
    struct TagItem *tstate;
    struct TagItem *tag;
    struct TagItem *clone;
    ULONG count = 0;
    ULONG i;

    tstate = original;
    while (NextTagItem(&tstate) != NULL)
        count++;

    /* +1 slot for the TAG_DONE terminator (MEMF_CLEAR zeroes it for us). */
    clone = AllocateTagItems(count + 1, base);
    if (!clone)
        return NULL;

    tstate = original;
    i = 0;
    while ((tag = NextTagItem(&tstate)) != NULL) {
        clone[i].ti_Tag = tag->ti_Tag;
        clone[i].ti_Data = tag->ti_Data;
        i++;
    }

    return clone;
}

/* ------------------------------------------------------------------------
 * RefreshTagItemClones -- rejuvenate a clone from the original. (V36)
 *
 * Autodoc: "If (and only if) the tag list 'clone' was created from
 * 'original' by CloneTagItems(), and if 'original' has not been changed in
 * any way, you can reset the clone list to its original state by using
 * this function." No reallocation -- clone's slot count is assumed
 * (per the precondition above) to already match original's flattened tag
 * count, so this just re-walks 'original' and overwrites clone's entries
 * in place, index for index.
 * ------------------------------------------------------------------------ */
VOID RefreshTagItemClones(struct TagItem *clone, struct TagItem *original)
{
    struct TagItem *tstate;
    struct TagItem *tag;
    ULONG i;

    if (!clone)
        return;

    tstate = original;
    i = 0;
    while ((tag = NextTagItem(&tstate)) != NULL) {
        clone[i].ti_Tag = tag->ti_Tag;
        clone[i].ti_Data = tag->ti_Data;
        i++;
    }
}

/* ========================================================================
 * PackBoolTags -- build a "flag" word from a tag list. (V36)
 *
 * Autodoc FUNCTION: "Picks out the boolean tag items in a tag list and
 * converts them into bit-flag representations according to a
 * correspondence defined by the tag list 'boolMap'. A boolean tag item is
 * one where only the logical value of the ti_Data is relevant. If this
 * field is 0, the value is FALSE, otherwise TRUE."
 *
 * Autodoc WARNING: "In a case where there is duplication of a tag in
 * tagList, the last of the identical tags will hold sway." -- satisfied
 * for free here: tagList is walked strictly forward via NextTagItem(), so
 * a later duplicate's set/clear is always applied after (and therefore
 * overrides) an earlier duplicate's.
 *
 * boolMap itself is a plain TagItem list (tag -> bit-mask pairing),
 * searched with FindTagItem() per tagList entry -- same list-walk
 * discipline as every other function in this file.
 * ======================================================================== */
ULONG PackBoolTags(ULONG initialFlags, struct TagItem *tagList, struct TagItem *boolMap)
{
    struct TagItem *tstate = tagList;
    struct TagItem *tag;
    struct TagItem *mapEntry;
    ULONG flags = initialFlags;

    while ((tag = NextTagItem(&tstate)) != NULL) {
        mapEntry = FindTagItem(tag->ti_Tag, boolMap);
        if (!mapEntry)
            continue; /* not one of boolMap's recognized boolean tags */

        if (tag->ti_Data)
            flags |= mapEntry->ti_Data;   /* TRUE  -> set the bit(s)   */
        else
            flags &= ~mapEntry->ti_Data;  /* FALSE -> clear the bit(s) */
    }

    return flags;
}

/* ========================================================================
 * FilterTagChanges -- eliminate tags which specify no change. (V36)
 *
 * Autodoc FUNCTION: "This function goes through changeList. For each item
 * found in changeList, if the item is also present in originalList, and
 * their data values are identical, then the tag is removed from
 * changeList. If the two tag's data values are different and the 'apply'
 * value is non-zero, then the tag data in originalList will be updated to
 * match the value from changeList."
 *
 * "removed from changeList" is realized the same way every other
 * list-mutating utility.library function represents removal: overwriting
 * ti_Tag with TAG_IGNORE in place (changeList's own EXAMPLE shows exactly
 * this: {ATTR_Size,"large"} becomes {TAG_IGNORE,"large"}). An item in
 * changeList whose tag does not appear in originalList at all is left
 * untouched (the autodoc only specifies behaviour for tags "also present
 * in originalList"), and originalList is never touched for such an item.
 * ======================================================================== */
VOID FilterTagChanges(struct TagItem *changeList, struct TagItem *originalList, ULONG apply)
{
    struct TagItem *tstate = changeList;
    struct TagItem *tag;
    struct TagItem *origTag;

    while ((tag = NextTagItem(&tstate)) != NULL) {
        origTag = FindTagItem(tag->ti_Tag, originalList);
        if (!origTag)
            continue; /* not present in originalList -- untouched */

        if (origTag->ti_Data == tag->ti_Data) {
            tag->ti_Tag = TAG_IGNORE; /* no change -- drop from changeList */
        } else if (apply) {
            origTag->ti_Data = tag->ti_Data;
        }
    }
}

/* ========================================================================
 * MapTags -- convert ti_Tag values in a list via map pairing. (V36)
 *
 * Autodoc FUNCTION: "If the ti_Tag field of an item in tagList appears as
 * ti_Tag in some item in mapList, overwrite ti_Tag with the corresponding
 * ti_Data from the map list." MAP_REMOVE_NOT_FOUND removes (TAG_IGNOREs)
 * any tagList item whose tag isn't in mapList; MAP_KEEP_NOT_FOUND leaves
 * such items untouched. "For safety and order independence [...] if you
 * attempt to map some tag to the value TAG_DONE, the value TAG_IGNORE
 * will be substituted instead." mapList itself is never modified.
 *
 * BUGS (autodoc): "Prior to V39, the mapType parameter did not work. The
 * function always behaved as if the parameter was set to
 * MAP_KEEP_NOT_FOUND." This project's baseline is V40 (post-V39), so the
 * fix is expected to be in effect -- implemented straightforwardly per
 * mapType below. Per the "oracle wins over the docs" rule,
 * this is EMPIRICALLY CONFIRMED against the real V40 ROM in
 * tests/utilitycheck.c's MapTags-RemoveNotFound case, not merely
 * assumed from the version gate -- see provenance/
 * utility-library-tag-filters.md for the exact oracle result.
 * ======================================================================== */
VOID MapTags(struct TagItem *tagList, struct TagItem *mapList, ULONG mapType)
{
    struct TagItem *tstate = tagList;
    struct TagItem *tag;
    struct TagItem *mapEntry;

    while ((tag = NextTagItem(&tstate)) != NULL) {
        mapEntry = FindTagItem(tag->ti_Tag, mapList);

        if (mapEntry) {
            Tag newTag = mapEntry->ti_Data;
            tag->ti_Tag = (newTag == TAG_DONE) ? TAG_IGNORE : newTag;
        } else if (mapType == MAP_REMOVE_NOT_FOUND) {
            tag->ti_Tag = TAG_IGNORE;
        }
        /* MAP_KEEP_NOT_FOUND (or anything else): leave tag->ti_Tag as-is. */
    }
}

/* ========================================================================
 * FilterTagItems -- remove selected items from a tag list. (V36)
 *
 * Autodoc FUNCTION: "Removes tag items from a tag list (by changing
 * ti_Tag to TAG_IGNORE) depending on whether its ti_Tag value is found
 * in an array of tag values. If the 'logic' parameter is TAGFILTER_AND,
 * then all items not appearing in 'tagArray' are excluded from
 * 'tagList'. If 'logic' is TAGFILTER_NOT, then items not found in
 * 'tagArray' are preserved, and the ones in the array are cast out."
 * RESULTS: "numValid - number of valid items left in resulting filtered
 * list."
 *
 * filterArray is explicitly documented (INPUTS) as "an array of tag
 * values, terminated by TAG_DONE, as specified in the documentation for
 * TagInArray()" -- i.e. the exact same plain Tag/ULONG array type
 * TagInArray() already takes, not a TagItem list. Implemented strictly
 * on top of NextTagItem() (for the tagList walk) and TagInArray() (for
 * the array-membership check), per this file's own reuse discipline --
 * no list-walk or array-scan logic is duplicated here.
 * ======================================================================== */
ULONG FilterTagItems(struct TagItem *tagList, Tag *filterArray, ULONG logic)
{
    struct TagItem *tstate = tagList;
    struct TagItem *tag;
    ULONG numValid = 0;
    BOOL inArray;

    while ((tag = NextTagItem(&tstate)) != NULL) {
        inArray = TagInArray(tag->ti_Tag, filterArray);

        if (logic == TAGFILTER_NOT) {
            /* Items found in the array are cast out; others survive. */
            if (inArray)
                tag->ti_Tag = TAG_IGNORE;
            else
                numValid++;
        } else {
            /* TAGFILTER_AND (and, per this project's own discipline of
             * not inventing undocumented behaviour, any other value is
             * treated the same way the autodoc's only other named
             * constant works): everything NOT in the array is
             * excluded. */
            if (!inArray)
                tag->ti_Tag = TAG_IGNORE;
            else
                numValid++;
        }
    }

    return numValid;
}

/* ========================================================================
 * ApplyTagChanges -- change a tag list based on a second tag list. (V39)
 *
 * Autodoc FUNCTION text: "For any tag that appears in both 'list' and
 * 'changeList', this function will change the ti_Data field of the tag
 * in 'list' to match the ti_Data field of the tag in 'changeList'. In
 * effect, 'changeList' contains a series of new values for tags already
 * in 'list'. Any tag in 'changeList' that is not in 'list' is ignored."
 * INPUTS: "list - a list of existing tags (may be NULL); changeList - a
 * list of tags to modify 'list' with (may be NULL)."
 *
 * Distinct from FilterTagChanges() (second work package, LVO -54) in one
 * specific, deliberately-verified detail, per this function's own SEE
 * ALSO cross-reference to FilterTagChanges(): FilterTagChanges() mutates
 * changeList itself (converting matched/identical entries' ti_Tag to
 * TAG_IGNORE), but ApplyTagChanges()'s own autodoc text never says
 * anything of the kind about changeList -- it only ever describes
 * writing into list's ti_Data. changeList is walked read-only here and
 * is NEVER written to, confirmed by this work package's own test case
 * (tests/utilitycheck.c's ApplyTagChanges-ChangeListUnmodified)
 * comparing changeList's bytes before and after the call.
 * ======================================================================== */
VOID ApplyTagChanges(struct TagItem *list, struct TagItem *changeList)
{
    struct TagItem *tstate = changeList;
    struct TagItem *ctag;
    struct TagItem *ltag;

    while ((ctag = NextTagItem(&tstate)) != NULL) {
        ltag = FindTagItem(ctag->ti_Tag, list);
        if (ltag)
            ltag->ti_Data = ctag->ti_Data;
        /* Not in list -> ignored (per autodoc), and changeList's own
         * entry is left completely untouched either way. */
    }
}
