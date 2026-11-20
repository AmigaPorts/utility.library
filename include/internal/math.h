/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <exec/types.h>

// A 64-bit return value travels in D0 (high) and D1 (low), which is exactly
// the quotient:remainder and high:low register pairing the API documents.
LONG SMult32(LONG arg1, LONG arg2);
ULONG UMult32(ULONG arg1, ULONG arg2);
signed long long SDivMod32(LONG dividend, LONG divisor);
unsigned long long UDivMod32(ULONG dividend, ULONG divisor);
signed long long SMult64(LONG arg1, LONG arg2);
unsigned long long UMult64(ULONG arg1, ULONG arg2);

// Shared with the date functions, which must not rely on compiler helpers.
ULONG UMult32Core(ULONG a, ULONG b);
unsigned long long UDivMod32Core(ULONG dividend, ULONG divisor);
