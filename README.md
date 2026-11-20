# Open Source reimplementation of `utility.library` for AmigaOS 3.x
[![CMake](https://github.com/amigaports/utility.library/actions/workflows/cmake.yml/badge.svg)](https://github.com/amigaports/utility.library/actions/workflows/cmake.yml)

An open-source replacement for Kickstart's `utility.library`, based on
AmigaOS 3.1 (V40) behavior and preserving the classic API and ABI.
Development is ongoing; see [compatibility](#compatibility) for what is tested.

## Why?

To liberate AmigaOS and open source it under a fair license, piece by piece.

## Building

Build from this directory with an m68k AmigaOS toolchain and CMake:

```sh
cmake -S . -B build -DM68K_CPU=custom -DM68K_FPU=custom -DM68K_CRT=custom \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=./install
cmake --build build --parallel
cmake --install build
```

`UTILITY_CPU` defaults to `68000`, so the result runs on every Amiga; select
`68020` or later for a build that may use the wider instruction set.

The project builds independently. It produces the library, generated call
bindings and a generated header for extension-ROM embedding. The module keeps
all state in its library base and has no writable sections, so the same file
works as a ROM resident and as a disk library in `LIBS:`.

## Development

The implementation is written from public documentation only: the AmigaOS NDK
autodocs and includes, and the ROM Kernel Reference Manuals. Where the
documentation and an original Kickstart 3.1 disagree, the original's observed
behavior wins. No original code was disassembled or copied.

Comments in the function bodies record which document specified each behavior
and which details were confirmed against the original. Some of them refer to
`provenance/` logs and a test rig of the tree the code was developed in; those
are not part of this repository.

### Testing

`tests/utilitycheck.c` is a differential test. It opens this implementation
and the system's resident `utility.library` side by side, calls every function
through both and compares the results. Because `OpenLibrary("utility.library")`
always finds the resident copy, the test loads this implementation under the
name `utility-new.library`:

```sh
cmake -S . -B build -DM68K_CPU=custom -DM68K_FPU=custom -DM68K_CRT=custom \
    -DUTILITY_BUILD_TESTS=ON
cmake --build build --parallel
```

Copy `build/tests/utility-new.library` to `LIBS:` and run
`build/tests/UtilityCheck` on the target. It reports one `PASS` or `FAIL` line
per case on the serial port and on standard output.

## Compatibility

✅ Matches tested original behavior; ❌ known difference or unsupported;
— not established. These are tested paths, not a claim of complete API parity.

| Functions | Status |
|---|:---:|
| Tag lists: find, iterate, clone, filter, map, apply | ✅ |
| PackBoolTags, PackStructureTags, UnpackStructureTags | ✅ |
| CallHookPkt | ✅ |
| Amiga2Date, Date2Amiga, CheckDate | ✅ |
| SMult32, UMult32, SDivMod32, UDivMod32 | ✅ |
| SMult64, UMult64 | ✅ |
| Stricmp, Strnicmp, ToUpper, ToLower | ✅ |
| Named objects and GetUniqueID | ✅ |
| AmigaOS 3.2 additions (VSNPrintf, Strncpy, Strncat, SDivMod64, UDivMod64) | ❌ |
| Operation as the system's ROM resident | — |

The comparisons were made against Kickstart 3.1 (V40) with this implementation
loaded from disk. Booting a system with it in place of the original resident
has not been tested yet.

## License

Library code is MPL-2.0. See [interface](HEADERS.md) and the accompanying
`LICENSE` file.
