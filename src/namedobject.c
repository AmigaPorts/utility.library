/* SPDX-License-Identifier: MPL-2.0 */
/* namedobject.c -- utility.library's V39 NamedObject family: AddNamedObject,
 * AllocNamedObjectA, AttemptRemNamedObject, FindNamedObject, FreeNamedObject,
 * NamedObjectName, ReleaseNamedObject, RemNamedObject, GetUniqueID (LVO -222
 * through -270, see vectors.c and HEADERS.md). Ninth work package
 * (provenance/utility-library-namedobject.md).
 *
 * ------------------------------------------------------------------------
 * THE CORE DESIGN PROBLEM, per <utility/name.h> verbatim:
 *
 *     struct NamedObject { APTR no_Object; };
 *
 * That one field is reserved ENTIRELY for the caller's own data (or, per
 * ANO_UserSpace, memory WE allocate on the caller's behalf and point
 * no_Object at) -- utility.doc never describes a "private" field on this
 * struct because there isn't one. Every bit of state this family actually
 * needs (name string, use count, priority, flags, namespace-list
 * membership, an attached child namespace, pending-removal tracking) has
 * to live in memory private to this implementation, keyed off the SAME
 * pointer AllocNamedObjectA() hands back and every other function takes
 * as `object`. This project already has exactly this pattern for its own
 * library base (struct UtilityLibrary in internal/library.h): a
 * private struct with the public struct as its first member, so a plain
 * pointer cast recovers the private view, and the public view the caller
 * holds is bit-for-bit a prefix of it.
 * ------------------------------------------------------------------------
 */

#include "internal/library.h"
#include "internal/namedobject.h"
#include "internal/tags.h"
#include "internal/string.h"

#include <exec/lists.h>
#include <exec/nodes.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <exec/semaphores.h>
#include <utility/tagitem.h>
#include <utility/name.h>

#define __NOLIBBASE__
#include <proto/exec.h>

#include <stddef.h>

/* ========================================================================
 * Design decision 1 -- the private struct and the root namespace.
 *
 * `struct PrivNamedObject` is this implementation's hidden superset of
 * the public struct, same "public struct is a prefix of a larger private
 * struct" idiom as struct UtilityLibrary. `pub` MUST be the
 * first member so `(struct NamedObject *)priv == (struct PrivNamedObject
 * *)pub` always holds -- no offset arithmetic needed at the public/private
 * boundary (only going the OTHER way, Node -> owning object, needs
 * offsetof(), see FindNamedObject below).
 *
 * `children`: every allocated object carries its own (always-initialized,
 * always-valid, possibly-empty) struct List. `hasChildSpace` (set from
 * ANO_NameSpace at allocation time) gates whether an object may actually
 * be used as someone else's `nameSpace` argument to AddNamedObject/
 * FindNamedObject -- see the PROVENANCE-CORRECTION note just below for
 * why this is now enforced, where it previously was not.
 *
 * PROVENANCE-CORRECTION (2026-10-13, caught in review -- see
 * provenance/utility-library-namedobject.md's own dated correction
 * section for the full account): this comment used to claim the real ROM
 * "imposes no such restriction either, confirmed by
 * `NamedObject-UseAsNamespaceWithoutAnoNameSpaceFlag`". That cited test
 * never existed anywhere in this project's history -- no such test was
 * ever written, and the permissive behaviour it claimed to confirm was
 * never actually oracle-tested at the time this comment was first
 * written. It was an unverified inference from the autodoc text
 * (reasonable on its face -- nothing in AddNamedObject's or
 * AllocNamedObjectA's own autodoc wording says passing a flagless object
 * as someone's `nameSpace` must fail) presented, incorrectly, as an
 * oracle-confirmed fact. A later, unrelated follow-up work package (the
 * one that added real multi-task locking/concurrency testing) stumbled
 * into the real answer by accident, via its own concurrency test's
 * namespace setup: with real Kickstart 3.1, every `AddNamedObject()` call
 * against a flagless namespace object failed outright (`okA=0 okB=0`).
 * This work package's own dedicated differential tests
 * (`AddNamedObject-NonNamespaceObject-FailsNoSideEffects`,
 * `FindNamedObject-NonNamespaceObject-ReturnsNull`, see utilitycheck.c)
 * confirm this directly and fix AddNamedObject below to match: a
 * non-NULL `nameSpace` argument that was NOT allocated with
 * `ANO_NameSpace=TRUE` is now rejected outright, consistent with the
 * real, confirmed oracle behaviour -- per the project's "the oracle wins
 * over the docs" rule. `hasChildSpace` is no longer merely an
 * inspectable record for AddNamedObject, which now branches on it.
 * FindNamedObject needed NO code change at all: tested separately (not
 * assumed to mirror AddNamedObject, per this project's own established
 * practice of checking rather than assuming symmetry between
 * superficially similar functions), it already returns NULL for a
 * flagless object on BOTH bases, simply because that object's own
 * `children` list is valid but empty -- no explicit precondition check
 * was ever needed there.
 *
 * The ROOT namespace (nameSpace == NULL to AddNamedObject/FindNamedObject)
 * has no NamedObject of its own to carry a `children` list or an
 * ANO_Flags value -- there is no publicly-documented "root NamedObject"
 * at all, just the NULL sentinel. This implementation represents it as a
 * single struct List in the library base (root_namespace), initialized once
 * from Init() (see init.c's call to NamedObjectsLibInit() below) and
 * scoped to this library's own open lifetime -- nothing in any of the 9
 * autodocs suggests the root namespace needs to survive CloseLibrary() or
 * be shared with any OTHER library instance/process beyond what a single
 * resident copy of this library's code already provides for the system.
 * A direct, unavoidable consequence: the root namespace carries no
 * ANO_Flags of its own, so NSF_NODUPS/NSF_CASE have no root-level setting
 * to read -- SpaceFlags() below treats the root as always
 * duplicates-allowed, case-insensitive-search (same as every other
 * namespace's search semantics per FindNamedObject's own "caseless, using
 * the current locale string comparison routines" wording, which doesn't
 * carve out an exception for the root).
 * ======================================================================== */
struct PrivNamedObject {
    struct NamedObject pub;        /* MUST be first -- see header comment */
    struct Node node;              /* used when Enqueue()d into some namespace's `children` list */
    struct List children;          /* this object's own namespace; always valid, see above */
    BOOL hasChildSpace;             /* ANO_NameSpace at alloc time -- AddNamedObject() now
                                       * enforces this as a precondition on its `nameSpace`
                                       * argument (provenance-integrity correction, 2026-10-13,
                                       * see this struct's own header comment); FindNamedObject()
                                       * does NOT check it (independently tested, found not to
                                       * need it -- same section) */
    BOOL inNamespace;               /* TRUE while linked into some namespace's `children` list */
    BOOL removed;                    /* TRUE once fully removed (useCount reached 0 via
                                       * AttemptRemNamedObject/RemNamedObject/ReleaseNamedObject) --
                                       * see the real-oracle correction in design decision 2's own
                                       * comment below: a SECOND AttemptRemNamedObject() on an
                                       * already-fully-removed object must fail, not silently
                                       * re-succeed just because useCount happens to already read 0 */
    ULONG anoFlags;                 /* ANO_Flags: NSF_NODUPS / NSF_CASE, this object's OWN flags
                                      * (relevant when THIS object is used as someone's nameSpace) */
    LONG useCount;                   /* open count; 1 at allocation (autodoc: "automatically has one use") */
    STRPTR name;                     /* AllocVec()'d copy of the name given at allocation */
    ULONG userSpaceSize;              /* 0 if ANO_UserSpace wasn't given; else the AllocMem()'d
                                       * no_Object block's size, needed by FreeNamedObject */
    struct Message *pendingRemoval;   /* non-NULL: a RemNamedObject() is waiting for useCount==0;
                                        * see design decision 2 below */
};

#define PRIV(obj) ((struct PrivNamedObject *)(obj))

static void InitEmptyList(struct List *l)
{
    l->lh_Head = (struct Node *)&l->lh_Tail;
    l->lh_Tail = NULL;
    l->lh_TailPred = (struct Node *)l;
    l->lh_Type = 0;
}


/* ========================================================================
 * Design decision 6 (added as a follow-up work package, 2026-10-13) --
 * locking.
 *
 * (The names below predate the move of all mutable state into the library
 * base: RootNameSpace is base->root_namespace, NamedObjectSemaphore is
 * base->named_object_semaphore and UniqueIDCounter is base->unique_id.)
 *
 * Real AmigaOS is genuinely multi-tasking, and this facility's own design
 * (the async RemNamedObject()/ReplyMsg() handshake) explicitly anticipates
 * one task holding a reference while another tries to remove the SAME
 * object -- concurrent access to the shared state below is this family's
 * documented use case, not a hypothetical worth hand-waving away. Three
 * kinds of shared, mutable state exist in this file: the single static
 * RootNameSpace list; each NamedObject's own `children` list (used as a
 * namespace once any object is passed as someone's `nameSpace` argument,
 * per design decision 1 above); and each object's own
 * useCount/pendingRemoval/removed/inNamespace fields.
 *
 * GRANULARITY: one single global `struct SignalSemaphore`
 * (NamedObjectSemaphore below), covering every namespace list AND every
 * object's state, rather than one semaphore per namespace (which would
 * also need one embedded in the synthetic root). A per-namespace scheme
 * buys more concurrency (unrelated namespaces no longer serialize against
 * each other) at a real cost: `AddNamedObject`/`FindNamedObject` already
 * touch two different "owners" in a single call in general (the namespace
 * list AND the object's own node/useCount), and `RemNamedObject` touches
 * the object's CURRENT namespace -- which namespace that is isn't fixed
 * at compile time, so a lock-ordering discipline (which semaphore is
 * "outer") would be needed to rule out deadlock between two tasks each
 * holding one namespace's lock and wanting the other's. That's a real
 * engineering cost for a facility whose own worst case (every call this
 * project's probe/golden tests exercise) touches at most a handful of
 * objects for microseconds -- not the kind of hot, highly-contended path
 * (e.g. graphics.library's own layer lock) that would justify the added
 * complexity and the added risk of getting the ordering wrong. One global
 * semaphore is simple enough to reason about completely and to actually
 * verify under real concurrency in the time available; reasoned through
 * and chosen over the more elaborate option, per this project's own
 * established style (see provenance's "Design decisions" section).
 *
 * WHAT'S INSIDE THE CRITICAL SECTION: every place that walks or mutates
 * RootNameSpace, any object's `children` list, or any object's
 * useCount/pendingRemoval/removed/inNamespace fields, for the duration of
 * that walk/mutation -- see each function's own ObtainSemaphore()/
 * ReleaseSemaphore() pair below. Two functions
 * (ReleaseNamedObject/RemNamedObject) can call ReplyMsg() as part of
 * their own logic; ReplyMsg() is deliberately called AFTER
 * ReleaseSemaphore() in both, never while the semaphore is held --
 * ReplyMsg() can make another, currently-Wait()ing task ready to run, and
 * while ReplyMsg() itself is documented as callable even from
 * Disable()/interrupts (so it would not, on its own, block the CALLING
 * task), holding a semaphore across it is still an unnecessary and
 * unjustifiable risk: it would serialize the reply against this
 * semaphore for no benefit (the message has already been fully prepared
 * by the time it's sent), and it sets a bad precedent for any future
 * maintainer tempted to add more logic after the ReplyMsg() while still
 * reading this function's "the lock is held here" as true. Both functions
 * below are restructured to compute what to do (and what message, if
 * any, to reply, with what ln_Name) WHILE holding the semaphore, release
 * it, and only then call ReplyMsg() -- the decision logic itself is
 * unchanged from the prior two work packages, just the point at which
 * ReplyMsg() physically runs relative to the lock.
 *
 * AllocNamedObjectA, FreeNamedObject, and NamedObjectName do NOT take the
 * semaphore: AllocNamedObjectA only initializes a just-allocated private
 * block nothing else can see yet (AllocMem/AllocVec already have their
 * own internal exec-level locking); NamedObjectName only reads the `name`
 * pointer, which is set once at allocation and never mutated again;
 * FreeNamedObject's own autodoc-documented caller obligations ("must not
 * be a member of any NameSpace", "no one may have it open other than
 * yourself") already rule out any legitimate CONCURRENT access to the
 * object being freed -- same unchecked-precondition style this file
 * already uses for FreeNamedObject elsewhere (see that function's own
 * header comment), not a new gap introduced by this work package.
 *
 * GetUniqueID's own Disable()/Enable() critical section (design decision
 * 5 above) protects an entirely independent piece of state
 * (UniqueIDCounter) that nothing in this locking scheme touches at all --
 * confirmed by inspection, not assumed: no function in this file reads or
 * writes UniqueIDCounter except GetUniqueID itself, and NamedObjectSemaphore
 * is never Obtain()ed from inside GetUniqueID's own Disable()/Enable()
 * pair or vice versa. Left exactly as-is; no coordination needed.
 * ======================================================================== */

/* Called once from init.c's Init(), which runs exactly once per load --
 * the natural place for one-time state, rather than a lazy "is it
 * initialized yet?" check on every call. */
void NamedObjectsLibInit(struct UtilityLibrary *base)
{
    struct ExecBase *SysBase = base->exec;
    InitEmptyList(&base->root_namespace);
    InitSemaphore(&base->named_object_semaphore);
}

static struct List *SpaceOf(struct NamedObject *nameSpace, struct UtilityLibrary *base)
{
    if (!nameSpace)
        return &base->root_namespace;
    return &PRIV(nameSpace)->children;
}

static ULONG SpaceFlags(struct NamedObject *nameSpace)
{
    if (!nameSpace)
        return 0;  /* root: no ANO_Flags storage exists -- see header comment */
    return PRIV(nameSpace)->anoFlags;
}

/* Local, non-exported, case-SENSITIVE name-equality helper -- only used
 * by AddNamedObject's own NSF_CASE duplicate check below. Deliberately
 * hand-rolled (no libc string.h dependency), same house style as tags.c's
 * own Stricmp/Strnicmp. */
static BOOL NamesEqualCase(STRPTR a, STRPTR b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (BOOL)(*a == *b);
}

static ULONG StrLenLocal(STRPTR s)
{
    ULONG n = 0;
    while (s[n])
        n++;
    return n;
}

static void StrCopyLocal(STRPTR dst, STRPTR src)
{
    while ((*dst++ = *src++) != 0)
        ;
}

static void UnlinkFromNamespace(struct PrivNamedObject *po, struct UtilityLibrary *base)
{
    struct ExecBase *SysBase = base->exec;
    if (po->inNamespace) {
        Remove(&po->node);
        po->inNamespace = FALSE;
    }
}

/* ------------------------------------------------------------------------
 * AddNamedObject -- add a named object to the given namespace. (V39)
 *
 * Autodoc FUNCTION: "Adds a new item to a NameSpace. If the NameSpace
 * doesn't support duplicate names, a search for a duplicate will be made,
 * and 0 (failure) will be returned. Otherwise, the entry will be
 * Enqueue()ed to the NameSpace." INPUTS: "nameSpace - the name space to
 * add to (NULL for root namespace). object - the object to add (If NULL,
 * will return failure)."
 *
 * "The NameSpace doesn't support duplicate names" reads as the
 * NAMESPACE's own ANO_Flags (NSF_NODUPS), read via SpaceFlags() above --
 * NOT any flag on the object being added (the object being added carries
 * no ANO_Flags of its own that mean anything to ITS membership; its
 * anoFlags field only matters if IT is later used as someone else's
 * nameSpace). Design decision for NSF_CASE, not pinned down by any
 * quoted autodoc text: FindNamedObject's own wording makes ordinary
 * lookups caseless ("using the current locale string comparison
 * routines"), so the duplicate check below defaults to Stricmp() too --
 * but NSF_CASE's own name ("default to caseless", i.e. the flag's
 * presence switches AWAY from the default) is read here as "when set on
 * the namespace, make the duplicate check case-SENSITIVE instead".
 * Flagged in provenance as an inference, not a quote -- confirmed or
 * corrected by this work package's own oracle testing.
 * ------------------------------------------------------------------------ */
/* Returns a full longword so no stale high word of D0 reaches the caller:
 * 0 on failure and, as the original does, -1 on success. */
LONG AddNamedObject(struct NamedObject *nameSpace, struct NamedObject *object, struct UtilityLibrary *base)
{
    struct ExecBase *SysBase = base->exec;
    struct List *space;
    struct PrivNamedObject *po;
    ULONG flags;

    if (!object)
        return FALSE;

    /* REAL-ORACLE-CONFIRMED FIX (2026-10-13, provenance-integrity
     * correction -- see namedobject.c's own "Design decision 1" header
     * comment and provenance/utility-library-namedobject.md's dated
     * correction section): a non-NULL `nameSpace` that was NOT allocated
     * with ANO_NameSpace=TRUE must be rejected outright, with no side
     * effects at all -- real Kickstart 3.1 fails every AddNamedObject()
     * call against such an object. Checked before anything else (no
     * list lookup, no Enqueue(), no semaphore even taken) so a rejected
     * call genuinely does nothing, mirroring the real ROM's observed
     * all-or-nothing failure. */
    if (nameSpace && !PRIV(nameSpace)->hasChildSpace)
        return FALSE;

    po = PRIV(object);
    space = SpaceOf(nameSpace, base);
    flags = SpaceFlags(nameSpace);

    ObtainSemaphore(&base->named_object_semaphore);

    if (flags & NSF_NODUPS) {
        struct Node *n;
        BOOL caseSensitive = (BOOL)((flags & NSF_CASE) != 0);

        for (n = space->lh_Head; n->ln_Succ; n = n->ln_Succ) {
            if (!n->ln_Name || !po->name)
                continue;
            if (caseSensitive) {
                if (NamesEqualCase((STRPTR)n->ln_Name, po->name)) {
                    ReleaseSemaphore(&base->named_object_semaphore);
                    return FALSE;
                }
            } else {
                if (Stricmp((STRPTR)n->ln_Name, po->name) == 0) {
                    ReleaseSemaphore(&base->named_object_semaphore);
                    return FALSE;
                }
            }
        }
    }

    Enqueue(space, &po->node);
    po->inNamespace = TRUE;

    ReleaseSemaphore(&base->named_object_semaphore);

    return -1;
}

/* ------------------------------------------------------------------------
 * AllocNamedObjectA -- allocate a named object. (V39)
 *
 * Autodoc FUNCTION/TAGS text quoted verbatim in the task-level comment
 * this work package was briefed from; see provenance for the full quote.
 * "name - name for the object (must not be NULL)" -- enforced below.
 * ------------------------------------------------------------------------ */
struct NamedObject *AllocNamedObjectA(STRPTR name, struct TagItem *tagList, struct UtilityLibrary *base)
{
    struct ExecBase *SysBase = base->exec;
    struct PrivNamedObject *po;
    BOOL wantSpace;
    ULONG userSize;
    LONG priority;
    ULONG flags;
    ULONG nameLen;

    if (!name)
        return NULL;

    /* Any non-zero value counts, not just one with low-word bits set --
     * confirmed against Kickstart 3.1 with ti_Data = 0x00010000. */
    wantSpace = (BOOL)(GetTagData(ANO_NameSpace, FALSE, tagList) != 0);
    userSize = GetTagData(ANO_UserSpace, 0, tagList);
    priority = (LONG)GetTagData(ANO_Priority, 0, tagList);
    flags = GetTagData(ANO_Flags, 0, tagList);

    po = AllocMem(sizeof(struct PrivNamedObject), MEMF_PUBLIC | MEMF_CLEAR);
    if (!po)
        return NULL;

    nameLen = StrLenLocal(name);
    po->name = AllocVec(nameLen + 1, MEMF_PUBLIC);
    if (!po->name) {
        FreeMem(po, sizeof(struct PrivNamedObject));
        return NULL;
    }
    StrCopyLocal(po->name, name);

    po->node.ln_Name = (char *)po->name;
    po->node.ln_Pri = (BYTE)priority;
    po->node.ln_Type = NT_USER;
    po->hasChildSpace = wantSpace;
    po->inNamespace = FALSE;
    po->removed = FALSE;
    po->anoFlags = flags;
    po->useCount = 1;  /* "automatically has one use" */
    po->userSpaceSize = 0;
    po->pendingRemoval = NULL;
    InitEmptyList(&po->children);
    po->pub.no_Object = NULL;

    if (userSize) {
        /* "size in bytes of long-word-aligned user memory" -- AllocMem()
         * already returns long-word-aligned blocks, but round the
         * REQUESTED size up too so a caller indexing past the nominal
         * size by up to 3 bytes (e.g. an array of a struct whose size
         * isn't itself a multiple of 4) still lands inside the block
         * rather than past it. */
        ULONG aligned = (userSize + 3UL) & ~3UL;
        APTR mem = AllocMem(aligned, MEMF_PUBLIC | MEMF_CLEAR);

        if (!mem) {
            FreeVec(po->name);
            FreeMem(po, sizeof(struct PrivNamedObject));
            return NULL;
        }
        po->pub.no_Object = mem;
        po->userSpaceSize = aligned;
    }

    return &po->pub;
}

/* ------------------------------------------------------------------------
 * Design decision 2 -- use-counting and the async removal handshake.
 *
 * AttemptRemNamedObject: "You must have found the object first (in order
 * to get a use count) before trying to remove it." -- the caller's own
 * reference (from AllocNamedObjectA's implicit use, or a prior
 * FindNamedObject) is consumed by a successful call, same as
 * ReleaseNamedObject would consume it, but ONLY if doing so would bring
 * the count all the way to zero in this one synchronous call (there is no
 * message parameter, so there is no way for this function to defer
 * anything -- "attempts" is read literally: it either finishes
 * immediately or it doesn't touch anything at all). If useCount > 1
 * (someone ELSE besides the caller also holds a reference), returning
 * FALSE WITHOUT decrementing is the only option that doesn't leak a use
 * count with no corresponding release. "or is in the process of being
 * removed" maps directly onto `pendingRemoval != NULL`.
 *
 * REAL-ORACLE CORRECTION (this work package's own golden-test recording,
 * real Kickstart 3.1): the first draft of this function treated an
 * object whose useCount already read 0 (because an EARLIER
 * AttemptRemNamedObject call had already fully removed it) the same as
 * "useCount == 1, remove now" -- i.e. it happily re-"succeeded" a second
 * time on an object that was already gone. `AttemptRemNamedObject-
 * NotInUse-ImmediateSuccess` failed against the real ROM specifically on
 * this second-call case: the real library returns FALSE for a repeat
 * call on an already-fully-removed object, not TRUE. The `removed` field
 * (set TRUE exactly once, by whichever of AttemptRemNamedObject/
 * RemNamedObject/ReleaseNamedObject is the one that brings useCount to 0
 * for the first time) fixes this: AttemptRemNamedObject now checks it
 * before anything else and fails outright if already set, independent of
 * what useCount itself currently reads. Treat this as confirming the
 * object pointer itself becomes an invalid handle for any further
 * removal-family call once it's fully removed -- same "don't call this
 * again after it's done" contract as, e.g., a second FreeMem() of the
 * same block, just surfaced here as a documented return value instead of
 * a memory-corruption bug.
 *
 * RemNamedObject: unlinks from its namespace IMMEDIATELY (so no NEW
 * FindNamedObject call can discover it after this point -- read as the
 * "removed from namespace" half of "fully removed"), then does its own
 * documented "effectively a ReleaseNamedObject()" for the caller's found
 * reference. If that brings the count to zero right there (the common
 * case: nobody else had a reference), the message is replied
 * IMMEDIATELY, ln_Name = the object pointer ("fully removed" = both
 * unlinked AND the last reference just dropped in the same call). If the
 * count does NOT reach zero (someone else still holds a reference), the
 * message is stashed in `pendingRemoval` and NOT replied yet --
 * ReleaseNamedObject (below) is the ONLY other place that checks
 * `pendingRemoval` and replies it, exactly when ITS OWN decrement brings
 * the count to zero. This is the autodoc's own documented split: "It
 * will reply the message when the object is fully removed" (RemNamedObject)
 * vs. "If the object has been removed, and the count goes to 0, the
 * remover will be notified" (ReleaseNamedObject) -- two different
 * functions, two different autodoc paragraphs, because the notification
 * can genuinely happen from either one depending on timing.
 *
 * Double-call / race case (RemNamedObject called again on an object that
 * already has a removal pending): this struct has room for exactly one
 * pending message. Per AttemptRemNamedObject's own "in the process of
 * being removed" wording as a real, anticipated state, a second
 * RemNamedObject() on the same object replies the NEW message right away
 * with ln_Name = NULL ("the object was removed by another process" --
 * quoting RemNamedObject's own INPUTS/RESULT text for exactly this
 * phrasing) rather than silently dropping it or clobbering the original
 * pending message, and still performs its own ReleaseNamedObject() for
 * the caller's found reference (every RemNamedObject call consumes
 * exactly one use count, pending-removal race or not).
 * ------------------------------------------------------------------------ */
LONG AttemptRemNamedObject(struct NamedObject *object, struct UtilityLibrary *base)
{
    struct ExecBase *SysBase = base->exec;
    struct PrivNamedObject *po;

    if (!object)
        return FALSE;

    po = PRIV(object);

    /* No ReplyMsg() anywhere in this function -- the whole decision and
     * mutation can simply happen under one ObtainSemaphore()/
     * ReleaseSemaphore() pair, no deferred-reply restructuring needed
     * (contrast RemNamedObject/ReleaseNamedObject below). */
    ObtainSemaphore(&base->named_object_semaphore);

    if (po->removed) {
        ReleaseSemaphore(&base->named_object_semaphore);
        return FALSE;           /* already fully removed by an earlier call -- real-oracle-
                                  * confirmed correction, see this function's own header
                                  * comment: a second AttemptRemNamedObject() on an
                                  * already-removed object must fail, not re-succeed just
                                  * because useCount still reads 0 */
    }
    if (!po->inNamespace) {
        ReleaseSemaphore(&base->named_object_semaphore);
        return FALSE;           /* SECOND real-oracle-confirmed correction (same golden-test
                                  * recording): "remove ... from whatever NameSpace it's in"
                                  * literally requires the object to BE in a namespace right
                                  * now -- an object that was never AddNamedObject()ed to
                                  * anything returns FALSE here on the real ROM
                                  * (AttemptRemNamedObject-NeverAdded: real=0), not TRUE just
                                  * because its use count happens to be low enough. This
                                  * project's own first draft didn't track namespace
                                  * membership as a precondition at all here, only use count. */
    }
    if (po->pendingRemoval) {
        ReleaseSemaphore(&base->named_object_semaphore);
        return FALSE;          /* already being removed elsewhere */
    }
    if (po->useCount > 1) {
        ReleaseSemaphore(&base->named_object_semaphore);
        return FALSE;          /* still in use by someone else -- don't touch the count */
    }

    po->useCount = 0;
    po->removed = TRUE;
    UnlinkFromNamespace(po, base);

    ReleaseSemaphore(&base->named_object_semaphore);

    return TRUE;
}

/* LOCKING NOTE (design decision 6, provenance's 2026-10-13 locking
 * section): the decision logic below is UNCHANGED from the prior two
 * work packages -- only restructured so every read/write of
 * po->removed/pendingRemoval/useCount/inNamespace happens under one
 * ObtainSemaphore()/ReleaseSemaphore() pair, and every ReplyMsg() (plus
 * the deferred-branch's own ReleaseNamedObject() call, which takes the
 * same semaphore itself) happens strictly AFTER ReleaseSemaphore(). Each
 * of the three branches below sets local `reply`/`replyName` (what
 * message to reply, and with what ln_Name) and/or `callRelease` (whether
 * to call ReleaseNamedObject() afterwards) while holding the lock, then
 * the lock is dropped, then those decisions are carried out -- in the
 * same relative order (reply first, then ReleaseNamedObject(), for the
 * pendingRemoval branch) the original single-pass code used. */
VOID RemNamedObject(struct NamedObject *object, struct Message *message, struct UtilityLibrary *base)
{
    struct ExecBase *SysBase = base->exec;
    struct PrivNamedObject *po;
    struct Message *reply = NULL;
    char *replyName = NULL;
    BOOL callRelease = FALSE;

    /* DEFENSIVE, NOT ORACLE-CONFIRMED (review finding, 2026-10-13 -- see
     * provenance/utility-library-namedobject.md's own follow-up section
     * for the full account): every sibling in this family
     * (AttemptRemNamedObject, ReleaseNamedObject, FreeNamedObject,
     * NamedObjectName) checks `object` for NULL before dereferencing it;
     * this function originally didn't. The autodoc's own INPUTS text for
     * `object` here ("Must be a valid NamedObject") reads the same as
     * FreeNamedObject's unchecked-caller-obligation precedent elsewhere
     * in this file, not as a documented NULL-tolerant case the way
     * ReleaseNamedObject's own "(No action if NULL)" explicitly is -- so
     * this guard is NOT claiming the real ROM was tested with a NULL
     * `object` and found to behave this way; it is added purely to match
     * this family's own established defensive-programming convention at
     * essentially zero cost. `message` is separately documented as
     * "must be supplied" by this same autodoc entry, i.e. a NULL message
     * is caller-contract territory the same way; left unguarded. */
    if (!object)
        return;

    po = PRIV(object);

    ObtainSemaphore(&base->named_object_semaphore);

    if (po->removed) {
        /* REAL-ORACLE FINDING (follow-up to this work package, see
         * provenance/utility-library-namedobject.md's follow-up section):
         * a second RemNamedObject() call on an object that's already
         * fully removed (removed==TRUE, pendingRemoval==NULL,
         * useCount==0 -- via a prior RemNamedObject, AttemptRemNamedObject,
         * or enough ReleaseNamedObject calls) is confirmed, against real
         * Kickstart 3.1, to reply the message immediately with ln_Name =
         * NULL -- the SAME "removed by another process" signal
         * RemNamedObject's own autodoc documents for the already-pending
         * race case just below, not a second real/phantom removal (the
         * object pointer is never reported a second time). First-draft
         * code had no check for `removed` here at all, so it fell through
         * to the "main" path below: pendingRemoval was NULL (nothing
         * actually pending), so it re-armed pendingRemoval, found
         * useCount already 0, and replied immediately with ln_Name = the
         * object pointer -- a phantom second "successful removal" of an
         * object that was already gone. Deliberately does NOT call
         * ReleaseNamedObject() here (unlike the pendingRemoval branch
         * below): there is no outstanding use count left to release once
         * an object is fully removed -- the pointer itself is an invalid
         * handle for the whole removal family from this point on, same
         * as AttemptRemNamedObject's own `removed` check above. */
        reply = message;
        replyName = NULL;
    } else if (po->pendingRemoval) {
        reply = message;
        replyName = NULL;
        callRelease = TRUE;
    } else {
        po->pendingRemoval = message;
        UnlinkFromNamespace(po, base);

        if (po->useCount > 0)
            po->useCount--;

        if (po->useCount == 0) {
            reply = po->pendingRemoval;
            po->pendingRemoval = NULL;
            po->removed = TRUE;
            replyName = (char *)object;
        }
    }

    ReleaseSemaphore(&base->named_object_semaphore);

    if (reply) {
        reply->mn_Node.ln_Name = replyName;
        ReplyMsg(reply);
    }

    if (callRelease)
        ReleaseNamedObject(object, base);
}

/* ------------------------------------------------------------------------
 * ReleaseNamedObject -- decrement the open count; notify a pending
 * remover if the count just reached zero. See design decision 2 above --
 * this is the OTHER place (besides RemNamedObject's own immediate-zero
 * path) that can reply a pending removal message.
 * ------------------------------------------------------------------------ */
VOID ReleaseNamedObject(struct NamedObject *object, struct UtilityLibrary *base)
{
    struct ExecBase *SysBase = base->exec;
    struct PrivNamedObject *po;
    struct Message *reply = NULL;

    if (!object)
        return;

    po = PRIV(object);

    /* Same locking pattern as RemNamedObject() above: mutate state under
     * the semaphore, then ReplyMsg() strictly after ReleaseSemaphore(). */
    ObtainSemaphore(&base->named_object_semaphore);

    if (po->useCount > 0)
        po->useCount--;

    if (po->useCount == 0 && po->pendingRemoval) {
        reply = po->pendingRemoval;
        po->pendingRemoval = NULL;
        po->removed = TRUE;
    }

    ReleaseSemaphore(&base->named_object_semaphore);

    if (reply) {
        reply->mn_Node.ln_Name = (char *)object;
        ReplyMsg(reply);
    }
}

/* ------------------------------------------------------------------------
 * FindNamedObject -- find the next object of a given name. (V39)
 *
 * "If name is NULL, then all objects will be matched." "If lastObject is
 * non-NULL, ... the search will start at the node after lastobject."
 * "The name comparison is caseless, using the current locale string
 * comparison routines" -- design decision 4: there is no locale.library
 * in this project's plain-disk test environment (same situation already
 * documented for ToUpper/ToLower's own NOTES text, provenance/
 * utility-library-stringops.md), so this reduces to this project's own
 * already-implemented Stricmp() -- reused directly, not reimplemented.
 *
 * Recovering the owning struct PrivNamedObject from a bare struct Node*
 * (the list only ever holds the `node` member's address, not the private
 * struct's own address) needs offsetof() -- `node` is NOT the first
 * member of struct PrivNamedObject (pub is, deliberately, so the
 * public/private cast the OTHER functions all rely on needs no offset at
 * all), so this is the one place in this file that needs the subtraction.
 * ------------------------------------------------------------------------ */
struct NamedObject *FindNamedObject(struct NamedObject *nameSpace, STRPTR name,
                                     struct NamedObject *lastObject, struct UtilityLibrary *base)
{
    struct ExecBase *SysBase = base->exec;
    struct List *space = SpaceOf(nameSpace, base);
    /* NSF_CASE applies to searches too, not only to AddNamedObject()'s
     * duplicate check -- confirmed against Kickstart 3.1. */
    BOOL caseSensitive = (BOOL)((SpaceFlags(nameSpace) & NSF_CASE) != 0);
    struct Node *n;
    struct NamedObject *result = NULL;

    /* Covers both the list walk (another task's concurrent Enqueue()/
     * Remove() on this same namespace must not be interleaved with this
     * walk) and the useCount++ on a match -- the same field
     * ReleaseNamedObject/RemNamedObject/AttemptRemNamedObject mutate
     * under this same semaphore. */
    ObtainSemaphore(&base->named_object_semaphore);

    if (lastObject)
        n = PRIV(lastObject)->node.ln_Succ;
    else
        n = space->lh_Head;

    for (; n && n->ln_Succ; n = n->ln_Succ) {
        if (!name || (n->ln_Name && (caseSensitive ? NamesEqualCase((STRPTR)n->ln_Name, name)
                                                   : Stricmp((STRPTR)n->ln_Name, name) == 0))) {
            struct PrivNamedObject *found =
                (struct PrivNamedObject *)((UBYTE *)n - offsetof(struct PrivNamedObject, node));

            found->useCount++;
            result = &found->pub;
            break;
        }
    }

    ReleaseSemaphore(&base->named_object_semaphore);

    return result;
}

/* ------------------------------------------------------------------------
 * FreeNamedObject -- frees a named object. (V39)
 *
 * Autodoc: "The item must not be a member of any NameSpace, and no one
 * may have it open other than yourself. ... Any additional space
 * allocated via the datasize parameter for AllocNamedObject() is also
 * released." These are documented CALLER obligations, not conditions
 * this function verifies (same unchecked-precondition style as
 * FreeTagItems() elsewhere in this library) -- violating them is the
 * caller's bug, not something this function defends against.
 * ------------------------------------------------------------------------ */
VOID FreeNamedObject(struct NamedObject *object, struct UtilityLibrary *base)
{
    struct ExecBase *SysBase = base->exec;
    struct PrivNamedObject *po;

    if (!object)
        return;

    po = PRIV(object);

    if (po->name)
        FreeVec(po->name);
    if (po->pub.no_Object && po->userSpaceSize)
        FreeMem(po->pub.no_Object, po->userSpaceSize);

    FreeMem(po, sizeof(struct PrivNamedObject));
}

/* ------------------------------------------------------------------------
 * NamedObjectName -- return the name of the object. (V39)
 * ------------------------------------------------------------------------ */
STRPTR NamedObjectName(struct NamedObject *object)
{
    if (!object)
        return NULL;
    return PRIV(object)->name;
}

/* ------------------------------------------------------------------------
 * Design decision 5 -- GetUniqueID's "safe in interrupts" claim.
 *
 * "Returns a unique value each time it is called. [...] This function is
 * safe in interrupts." A plain `return ++counter;` on a static global is
 * NOT safe against an interrupt landing between the load and the
 * store of a non-atomic read-modify-write on the 68000/68010 this
 * project targets (no single-instruction atomic increment-and-fetch
 * exists for an arbitrary ULONG on that CPU family). Disable()/Enable()
 * is exec.library's own documented general-purpose mechanism for exactly
 * this: a short, nestable, interrupt-safe critical section (RKRM
 * Libraries' own Exec chapter; also directly usable from interrupt code
 * itself, unlike a Forbid()/Permit() task-switch-only lock, which doesn't
 * help against a genuine CPU interrupt). The critical section here is
 * about as short as such a thing ever gets (one increment, one read
 * back) -- no other library code in this project currently needs
 * Disable()/Enable() at all, so this is GetUniqueID's own, narrowly-
 * scoped use of it, not a new shared idiom.
 * ------------------------------------------------------------------------ */

ULONG GetUniqueID(struct UtilityLibrary *base)
{
    struct ExecBase *SysBase = base->exec;
    ULONG id;

    Disable();
    id = ++base->unique_id;
    Enable();

    return id;
}
