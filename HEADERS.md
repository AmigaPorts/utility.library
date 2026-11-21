# Public interface

`sfd/utility_lib.sfd` generates the call bindings (`clib/`, `proto/`,
`inline/`, `pragmas/`). Private declarations stay in `include/internal/` and
are not installed.

The SFD covers the AmigaOS 3.1 (V40) interface through `GetUniqueID` at -270:
38 public functions, three reserved slots and one tag-call alias
(`AllocNamedObject`), following the four library lifecycle vectors. The
functions added in AmigaOS 3.2 (V47), from `VSNPrintf` at -312, are not
implemented.

| Offset | Function | Registers |
|---:|---|---|
| -30 | FindTagItem | D0, A0 |
| -36 | GetTagData | D0, D1, A0 |
| -42 | PackBoolTags | D0, A0, A1 |
| -48 | NextTagItem | A0 |
| -54 | FilterTagChanges | A0, A1, D0 |
| -60 | MapTags | A0, A1, D0 |
| -66 | AllocateTagItems | D0 |
| -72 | CloneTagItems | A0 |
| -78 | FreeTagItems | A0 |
| -84 | RefreshTagItemClones | A0, A1 |
| -90 | TagInArray | D0, A0 |
| -96 | FilterTagItems | A0, A1, D0 |
| -102 | CallHookPkt | A0, A2, A1 |
| -108, -114 | reserved | |
| -120 | Amiga2Date | D0, A0 |
| -126 | Date2Amiga | A0 |
| -132 | CheckDate | A0 |
| -138 | SMult32 | D0, D1 |
| -144 | UMult32 | D0, D1 |
| -150 | SDivMod32 | D0, D1 |
| -156 | UDivMod32 | D0, D1 |
| -162 | Stricmp | A0, A1 |
| -168 | Strnicmp | A0, A1, D0 |
| -174 | ToUpper | D0 |
| -180 | ToLower | D0 |
| -186 | ApplyTagChanges | A0, A1 |
| -192 | reserved | |
| -198 | SMult64 | D0, D1 |
| -204 | UMult64 | D0, D1 |
| -210 | PackStructureTags | A0, A1, A2 |
| -216 | UnpackStructureTags | A0, A1, A2 |
| -222 | AddNamedObject | A0, A1 |
| -228 | AllocNamedObjectA | A0, A1 |
| -234 | AttemptRemNamedObject | A0 |
| -240 | FindNamedObject | A0, A1, A2 |
| -246 | FreeNamedObject | A0 |
| -252 | NamedObjectName | A0 |
| -258 | ReleaseNamedObject | A0 |
| -264 | RemNamedObject | A0, A1 |
| -270 | GetUniqueID | |

The math functions do not need the library base in A6 and preserve all
address registers, including A0 and A1. The divide functions return the
quotient in D0 and the remainder in D1. The 64-bit multiplies return different
registers per CPU, as the original does: on a 68020 or later the exact product
with the low half in D0 and the high half in D1; on a 68000 or 68010 the high
half in D0 and the low half in D1, where a negative product is not the exact
two's complement value. The CPU is read from exec's `AttnFlags` at call time,
so one binary serves both.

Vector order and register assignments were taken as interface facts from
AROS's [`utility.conf`](https://github.com/aros-development-team/AROS/blob/master/rom/utility/utility.conf)
and checked against the function synopses in the AmigaOS NDK 3.2 autodocs. The
SFD itself was written for this project and is MPL-2.0.

No public headers are shipped. Building the library and its clients requires
the `utility/*.h` structure headers (and Exec's) from an Amiga SDK; AmigaOS
NDK headers are not redistributed.
