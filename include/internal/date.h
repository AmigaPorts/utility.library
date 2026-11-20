/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <exec/types.h>
#include <utility/date.h>

VOID Amiga2Date(ULONG seconds, struct ClockData* result);
ULONG Date2Amiga(struct ClockData* date);
ULONG CheckDate(struct ClockData* date);
