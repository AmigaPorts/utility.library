/* SPDX-License-Identifier: MPL-2.0 */
/* Checks the system's own utility.library named-object allocation, with no
 * second library involved: one run against Kickstart 3.1 shows that
 * FreeNamedObject() does not give the object's memory back, and that in
 * some memory layouts the machine then ends in exec's dead-end alert
 * 0x81000005 (memory list corrupted) and reboots. Run it on each Kickstart
 * to be compared, with the same output line format:
 *
 *   ROMNO utility.library 40.<rev>
 *   ROMNO leak len=1 allocated=40 returned=0
 *   FAIL FreeNamedObject-ReturnsTheObjectsMemory
 *   ROMNO soak n=25
 *   ...
 *   PASS NamedObjectSoak-HeapStaysConsistent
 *   DONE pass=1 fail=1
 *
 * The documented behaviour (utility.doc, FreeNamedObject: "Free one of a
 * number of structures used by utility.library") is that the object's memory
 * is released, so the first check expects every byte back. If the machine
 * reboots during the soak, the output simply stops after the last printed
 * line; the crash depends on the heap layout, so a different build or one
 * more output line can move or hide it.
 *
 * Output goes to serial.device through one request that is opened once, so
 * printing allocates nothing after the first line, even when exec's free
 * list is damaged.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <exec/libraries.h>
#include <exec/io.h>
#include <devices/serial.h>
#include <utility/tagitem.h>
#include <utility/name.h>
#include <proto/exec.h>
#include <clib/alib_protos.h>

static struct Library *g_utility;
static struct MsgPort *g_port;
static struct IOExtSer *g_serial;
static ULONG g_pass, g_fail;

struct Buffer {
    UBYTE *pos;
    UBYTE *end;
};

static void put_char(register UBYTE ch __asm("d0"), register struct Buffer *buffer __asm("a3"))
{
    if (buffer->pos < buffer->end) {
        *buffer->pos++ = ch;
    }
}

static void print(CONST_STRPTR format, ULONG *args)
{
    UBYTE line[160];
    struct Buffer buffer;

    buffer.pos = line;
    buffer.end = line + sizeof(line) - 1;
    RawDoFmt(format, args, (void (*)())put_char, &buffer);

    if (!g_serial) {
        g_port = CreatePort(NULL, 0);
        g_serial = (struct IOExtSer *)CreateExtIO(g_port, sizeof(struct IOExtSer));
        OpenDevice((CONST_STRPTR) "serial.device", 0, (struct IORequest *)g_serial, 0);
    }

    g_serial->IOSer.io_Command = CMD_WRITE;
    g_serial->IOSer.io_Length = (LONG)(buffer.pos - line);
    g_serial->IOSer.io_Data = line;
    DoIO((struct IORequest *)g_serial);
    g_serial->IOSer.io_Data = NULL;
}

static void verdict(CONST_STRPTR name, BOOL ok)
{
    ULONG args[1];

    args[0] = (ULONG)(APTR)name;
    print(ok ? (CONST_STRPTR) "PASS %s\r\n" : (CONST_STRPTR) "FAIL %s\r\n", args);

    if (ok) {
        g_pass++;
    } else {
        g_fail++;
    }
}

/* Library calls by hand: A0 and A1 are scratch for the callee, so they are
 * declared in/out here, as are D0 and D1. */
static struct NamedObject *alloc_named_object(STRPTR name)
{
    register ULONG result __asm("d0");
    register APTR a0 __asm("a0") = name;
    register APTR a1 __asm("a1") = NULL;
    register struct Library *a6 __asm("a6") = g_utility;

    __asm volatile("jsr -228(%%a6)" : "=r"(result), "+r"(a0), "+r"(a1) : "r"(a6) : "d1", "cc", "memory");

    return (struct NamedObject *)result;
}

static void free_named_object(struct NamedObject *object)
{
    register APTR a0 __asm("a0") = object;
    register struct Library *a6 __asm("a6") = g_utility;

    __asm volatile("jsr -246(%%a6)" : "+r"(a0) : "r"(a6) : "d0", "d1", "a1", "cc", "memory");
}

static ULONG available(void)
{
    ULONG bytes;

    Forbid();
    bytes = AvailMem(MEMF_PUBLIC);
    Permit();

    return bytes;
}

/* Walks exec's free lists through <exec/memory.h>, allocating nothing. */
static BOOL heap_consistent(void)
{
    struct MemHeader *header;
    BOOL ok = TRUE;

    Forbid();

    for (header = (struct MemHeader *)SysBase->MemList.lh_Head; ok && header->mh_Node.ln_Succ;
         header = (struct MemHeader *)header->mh_Node.ln_Succ) {
        struct MemChunk *chunk = header->mh_First;
        ULONG lower = (ULONG)header->mh_Lower, upper = (ULONG)header->mh_Upper;

        while (chunk) {
            ULONG at = (ULONG)chunk, bytes = chunk->mc_Bytes, next = (ULONG)chunk->mc_Next;

            if (at < lower || at >= upper || (at & 7) || bytes == 0 || (bytes & 7) || at + bytes > upper
                || (next && next < at + bytes)) {
                ok = FALSE;
                break;
            }

            chunk = chunk->mc_Next;
        }
    }

    Permit();

    return ok;
}

static void make_name(UBYTE* buffer, ULONG length)
{
    static const char alphabet[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKL";
    ULONG i;

    for (i = 0; i < length; i++) {
        buffer[i] = (UBYTE)alphabet[i];
    }

    buffer[length] = 0;
}

/* A freshly allocated, never added object has one use (its creator's). The
 * autodoc says FreeNamedObject() releases it. */
static void check_free_returns_memory(void)
{
    UBYTE name[48];
    ULONG length, args[4], before, allocated, after, leaked_lengths = 0;

    for (length = 1; length <= 40; length++) {
        struct NamedObject *object;

        make_name(name, length);
        before = available();
        object = alloc_named_object((STRPTR)name);
        allocated = before - available();

        if (length <= 2) {
            args[0] = length;
            args[1] = (ULONG)object;
            args[2] = allocated;
            print((CONST_STRPTR) "ROMNO alloc len=%ld object=%lx allocated=%ld\r\n", args);
        }

        free_named_object(object);
        after = available();

        if (after != before) {
            leaked_lengths++;
        }

        if (length == 1 || length == 8 || length == 40) {
            args[0] = length;
            args[1] = allocated;
            args[2] = after - (before - allocated);
            print((CONST_STRPTR) "ROMNO leak len=%ld allocated=%ld returned=%ld\r\n", args);
        }
    }

    args[0] = leaked_lengths;
    print((CONST_STRPTR) "ROMNO name lengths that leaked: %ld of 40\r\n", args);
    verdict((CONST_STRPTR) "FreeNamedObject-ReturnsTheObjectsMemory", (BOOL)(leaked_lengths == 0));
}

static void check_soak(void)
{
    UBYTE name[48];
    ULONG n, args[1];
    BOOL ok = TRUE;

    for (n = 1; n <= 300 && ok; n++) {
        struct NamedObject *object;

        make_name(name, (n % 40) + 1);
        object = alloc_named_object((STRPTR)name);
        free_named_object(object);
        ok = heap_consistent();

        if ((n % 25) == 0) {
            args[0] = n;
            print((CONST_STRPTR) "ROMNO soak n=%ld\r\n", args);
        }
    }

    verdict((CONST_STRPTR) "NamedObjectSoak-HeapStaysConsistent", ok);
}

int main(void)
{
    ULONG args[3];

    g_utility = OpenLibrary((CONST_STRPTR) "utility.library", 39);

    if (!g_utility) {
        return 20;
    }

    args[0] = g_utility->lib_Version;
    args[1] = g_utility->lib_Revision;
    print((CONST_STRPTR) "ROMNO utility.library %ld.%ld\r\n", args);

    check_free_returns_memory();
    check_soak();

    args[0] = g_pass;
    args[1] = g_fail;
    print((CONST_STRPTR) "DONE pass=%ld fail=%ld\r\n", args);

    CloseLibrary(g_utility);

    return g_fail ? 10 : 0;
}
