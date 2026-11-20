/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <exec/types.h>
#include <exec/ports.h>
#include <utility/tagitem.h>
#include <utility/name.h>

struct UtilityLibrary;

// Initializes the root name space and its lock; called once from Init.
void NamedObjectsLibInit(struct UtilityLibrary* base);

BOOL AddNamedObject(struct NamedObject* nameSpace, struct NamedObject* object, struct UtilityLibrary* base);
struct NamedObject* AllocNamedObjectA(STRPTR name, struct TagItem* tagList, struct UtilityLibrary* base);
LONG AttemptRemNamedObject(struct NamedObject* object, struct UtilityLibrary* base);
struct NamedObject* FindNamedObject(struct NamedObject* nameSpace, STRPTR name, struct NamedObject* lastObject,
    struct UtilityLibrary* base);
VOID FreeNamedObject(struct NamedObject* object, struct UtilityLibrary* base);
STRPTR NamedObjectName(struct NamedObject* object);
VOID ReleaseNamedObject(struct NamedObject* object, struct UtilityLibrary* base);
VOID RemNamedObject(struct NamedObject* object, struct Message* message, struct UtilityLibrary* base);
ULONG GetUniqueID(struct UtilityLibrary* base);
