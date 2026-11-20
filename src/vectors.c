/* SPDX-License-Identifier: MPL-2.0 */
// SDK vector order, including the reserved slots. MakeLibrary() builds the
// jump table from this array; see entry.c for the register-to-stack stubs.
#include "internal/library.h"

extern void FindTagItemEntry(void);
extern void GetTagDataEntry(void);
extern void PackBoolTagsEntry(void);
extern void NextTagItemEntry(void);
extern void FilterTagChangesEntry(void);
extern void MapTagsEntry(void);
extern void AllocateTagItemsEntry(void);
extern void CloneTagItemsEntry(void);
extern void FreeTagItemsEntry(void);
extern void RefreshTagItemClonesEntry(void);
extern void TagInArrayEntry(void);
extern void FilterTagItemsEntry(void);
extern void CallHookPktEntry(void);
extern void Amiga2DateEntry(void);
extern void Date2AmigaEntry(void);
extern void CheckDateEntry(void);
extern void SMult32Entry(void);
extern void UMult32Entry(void);
extern void SDivMod32Entry(void);
extern void UDivMod32Entry(void);
extern void StricmpEntry(void);
extern void StrnicmpEntry(void);
extern void ToUpperEntry(void);
extern void ToLowerEntry(void);
extern void ApplyTagChangesEntry(void);
extern void SMult64Entry(void);
extern void UMult64Entry(void);
extern void PackStructureTagsEntry(void);
extern void UnpackStructureTagsEntry(void);
extern void AddNamedObjectEntry(void);
extern void AllocNamedObjectAEntry(void);
extern void AttemptRemNamedObjectEntry(void);
extern void FindNamedObjectEntry(void);
extern void FreeNamedObjectEntry(void);
extern void NamedObjectNameEntry(void);
extern void ReleaseNamedObjectEntry(void);
extern void RemNamedObjectEntry(void);
extern void GetUniqueIDEntry(void);

const APTR utilityFunctions[UTILITY_VECTOR_COUNT + 1] = {
    (APTR)utilityOpen,     // -6
    (APTR)utilityClose,    // -12
    (APTR)utilityExpunge,  // -18
    (APTR)utilityReserved, // -24
    (APTR)FindTagItemEntry, // -30 FindTagItem
    (APTR)GetTagDataEntry, // -36 GetTagData
    (APTR)PackBoolTagsEntry, // -42 PackBoolTags
    (APTR)NextTagItemEntry, // -48 NextTagItem
    (APTR)FilterTagChangesEntry, // -54 FilterTagChanges
    (APTR)MapTagsEntry, // -60 MapTags
    (APTR)AllocateTagItemsEntry, // -66 AllocateTagItems
    (APTR)CloneTagItemsEntry, // -72 CloneTagItems
    (APTR)FreeTagItemsEntry, // -78 FreeTagItems
    (APTR)RefreshTagItemClonesEntry, // -84 RefreshTagItemClones
    (APTR)TagInArrayEntry, // -90 TagInArray
    (APTR)FilterTagItemsEntry, // -96 FilterTagItems
    (APTR)CallHookPktEntry, // -102 CallHookPkt
    (APTR)utilityReservedSlot, // -108 reserved
    (APTR)utilityReservedSlot, // -114 reserved
    (APTR)Amiga2DateEntry, // -120 Amiga2Date
    (APTR)Date2AmigaEntry, // -126 Date2Amiga
    (APTR)CheckDateEntry, // -132 CheckDate
    (APTR)SMult32Entry, // -138 SMult32
    (APTR)UMult32Entry, // -144 UMult32
    (APTR)SDivMod32Entry, // -150 SDivMod32
    (APTR)UDivMod32Entry, // -156 UDivMod32
    (APTR)StricmpEntry, // -162 Stricmp
    (APTR)StrnicmpEntry, // -168 Strnicmp
    (APTR)ToUpperEntry, // -174 ToUpper
    (APTR)ToLowerEntry, // -180 ToLower
    (APTR)ApplyTagChangesEntry, // -186 ApplyTagChanges
    (APTR)utilityReservedSlot, // -192 reserved
    (APTR)SMult64Entry, // -198 SMult64
    (APTR)UMult64Entry, // -204 UMult64
    (APTR)PackStructureTagsEntry, // -210 PackStructureTags
    (APTR)UnpackStructureTagsEntry, // -216 UnpackStructureTags
    (APTR)AddNamedObjectEntry, // -222 AddNamedObject
    (APTR)AllocNamedObjectAEntry, // -228 AllocNamedObjectA
    (APTR)AttemptRemNamedObjectEntry, // -234 AttemptRemNamedObject
    (APTR)FindNamedObjectEntry, // -240 FindNamedObject
    (APTR)FreeNamedObjectEntry, // -246 FreeNamedObject
    (APTR)NamedObjectNameEntry, // -252 NamedObjectName
    (APTR)ReleaseNamedObjectEntry, // -258 ReleaseNamedObject
    (APTR)RemNamedObjectEntry, // -264 RemNamedObject
    (APTR)GetUniqueIDEntry, // -270 GetUniqueID
    (APTR)-1,
};
