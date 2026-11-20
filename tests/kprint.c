/* SPDX-License-Identifier: MPL-2.0 */
#include "kprint.h"

#include <exec/io.h>
#include <exec/errors.h>
#include <devices/serial.h>
#include <proto/exec.h>
#include <clib/alib_protos.h>

struct StuffBuf {
    UBYTE *pos;
    UBYTE *end;
};

/* PutChProc for RawDoFmt: called as PutChProc(Char, PutChData) in D0/A3
 * (exec.library/RawDoFmt, exec.doc). The Amiga GCC target honours __asm
 * register-parameter attributes directly, so no hand-written trampoline
 * is needed to match that raw calling convention. */
static void StuffChar(register UBYTE ch __asm("d0"),
                      register struct StuffBuf *buf __asm("a3"))
{
    if (buf->pos < buf->end)
        *buf->pos++ = ch;
}

static LONG serial_write(UBYTE *data, ULONG len)
{
    struct MsgPort *port;
    struct IOExtSer *ioser;
    LONG result = -1;

    port = CreatePort(NULL, 0);
    if (!port)
        return -1;

    ioser = (struct IOExtSer *)CreateExtIO(port, sizeof(struct IOExtSer));
    if (ioser) {
        if (OpenDevice((CONST_STRPTR) "serial.device", 0,
                       (struct IORequest *)ioser, 0) == 0) {
            ioser->IOSer.io_Command = CMD_WRITE;
            ioser->IOSer.io_Length = len;
            ioser->IOSer.io_Data = (APTR)data;
            result = DoIO((struct IORequest *)ioser);
            CloseDevice((struct IORequest *)ioser);
        }
        DeleteExtIO((struct IORequest *)ioser);
    }
    DeletePort(port);
    return result;
}

void kprintf(CONST_STRPTR fmt, APTR args)
{
    UBYTE line[256];
    struct StuffBuf buf;

    buf.pos = line;
    buf.end = line + sizeof(line) - 1; /* room for RawDoFmt's trailing NUL */
    RawDoFmt(fmt, args, (void (*)())StuffChar, &buf);

    serial_write(line, (ULONG)(buf.pos - line));
}
