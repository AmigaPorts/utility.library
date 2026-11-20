/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <exec/types.h>
#include <utility/tagitem.h>

ULONG PackStructureTags(APTR pack, ULONG* packTable, struct TagItem* tagList);
ULONG UnpackStructureTags(APTR pack, ULONG* packTable, struct TagItem* tagList);
