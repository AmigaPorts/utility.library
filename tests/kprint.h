/* SPDX-License-Identifier: MPL-2.0 */
/* A from-spec KPrintF-workalike for on-target probes.
 *
 * debug.lib's KPrintF ("print formatted data to the console, defaults to
 * the serial port at 9600 baud" -- NDK 3.2 autodocs, debug_lib.doc) is
 * exactly the single-call debug-output convention this harness wants,
 * but debug.lib itself is a Commodore/Hyperion NDK binary archive, not
 * something to link against. Its documented *contract*
 * is public, though, and is trivially reproducible from two calls this
 * project already specs from public autodocs: exec.library/RawDoFmt
 * (formatting, no I/O of its own) and serial.device/CMD_WRITE (transmit).
 * kprintf() below is that reproduction -- same observable behaviour,
 * written fresh.
 */
#ifndef HARNESS_KPRINT_H
#define HARNESS_KPRINT_H

#include <exec/types.h>

/* args points at a ULONG array matching RawDoFmt's %-placeholder
 * convention (exec.doc) -- not true C varargs, kept explicit so the
 * calling convention needs no ABI assumptions beyond what's documented. */
void kprintf(CONST_STRPTR fmt, APTR args);

#endif
