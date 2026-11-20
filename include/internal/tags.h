/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <exec/types.h>
#include <utility/tagitem.h>

struct UtilityLibrary;

struct TagItem* NextTagItem(struct TagItem** tagItemPtr);
struct TagItem* FindTagItem(Tag tagValue, struct TagItem* tagList);
ULONG GetTagData(Tag tagValue, ULONG defaultVal, struct TagItem* tagList);
BOOL TagInArray(Tag tagValue, Tag* tagArray);
struct TagItem* AllocateTagItems(ULONG numTags, struct UtilityLibrary* base);
VOID FreeTagItems(struct TagItem* tagList, struct UtilityLibrary* base);
struct TagItem* CloneTagItems(struct TagItem* original, struct UtilityLibrary* base);
VOID RefreshTagItemClones(struct TagItem* clone, struct TagItem* original);
ULONG PackBoolTags(ULONG initialFlags, struct TagItem* tagList, struct TagItem* boolMap);
VOID FilterTagChanges(struct TagItem* changeList, struct TagItem* originalList, ULONG apply);
VOID MapTags(struct TagItem* tagList, struct TagItem* mapList, ULONG mapType);
ULONG FilterTagItems(struct TagItem* tagList, Tag* filterArray, ULONG logic);
VOID ApplyTagChanges(struct TagItem* list, struct TagItem* changeList);
