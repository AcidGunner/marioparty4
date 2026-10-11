# Mario Party 4 decomp: port bring-up (host first, PS3 later)

Goal: get the decompiled game logic running against a replaceable platform layer so a PS3 backend
only has to implement the layer, not touch game code. Everything here is additive except two
`#ifdef TARGET_PC` edits (`src/game/malloc.c`) and the include shims in `port/host/include`.

You need your own copy of the game. This repo has no assets; four engine files `#include` binary
`.inc` data extracted from the disc.

## Layout

    port/host/include/port_compat.h      force-included: __declspec(weak), 3-arg GXSetArray shim
    port/host/include/dolphin/os/OSFastCast.h   C replacement for the psq_l/psq_st asm
    port/host/include/port_log.h         PORT_STUB(): logs which stubs the game hits at exit
    port/host/src/mtx_host.c             portable C for the 27 PSMTX/PSVEC/C_MTX* the game calls (tested)
    port/host/src/sdk_host.c             real: time, arena (MAP_32BIT), OSReport/Panic, vsync, pad, render modes
    port/host/src/sdk_stubs_generated.c  logging no-ops for ~200 GX/OS/DVD/AR/AI/CARD/THP/msm functions
    port/host/tools/gen_stubs.py         regenerates the above from the repo's own headers
    port/host/check.sh | Makefile        syntax-check / build / link report

    make -f port/host/Makefile test      math unit tests
    make -f port/host/Makefile check     host-gcc syntax check of src/game and src/REL
    make -f port/host/Makefile link      builds + links, writes build/host/unresolved.txt

## Status (host gcc, 64-bit, USA 1.1 = VERSION 1)

- src/game: 78 of 80 files compile (`jmp.c`, `kerent.c` skipped on purpose); src/REL: 257 of 264
- links with 0 unresolved symbols
- real implementations: math, time, arena + OS heaps, vsync, file-backed DVD, process fibers
- the binary boots through memory init, creates the game processes, runs the object manager and then asks
  to link `dll/bootdll.rel` (see blocker 1)
- `make -f port/host/Makefile test` runs math, fiber-scheduler and DVD tests

## Disc data

`DVDOpen`/`DVDConvertPathToEntrynum` read from a directory holding the contents of the disc's `files/` tree:
`data/*.bin` (about 140 files, all listed in `include/datadir_table.h`; startup aborts with
`data.c: Data File Error(<name>)` if one is missing), `dll/*.rel`, `meminfo.bin`, movies if present.
Names are matched case-insensitively. Root is searched in this order: `$PORT_DISC_ROOT`, `/dev_usb000/MP4/`,
`/dev_usb001/MP4/`, `/dev_hdd0/MP4/`, `/dev_hdd0/game/MP4PORT01/USRDIR/disc/`, `./gamedata/`, `./disc/`;
a root counts only if it contains `data/board.bin`. The chosen root is printed at startup.

## Known blockers, in the order you will hit them

1. **Overlays (REL) are loaded dynamically** (`OSLink` of `dll/*.rel` in the objdll code). Needs static linking of
   `src/REL/*` plus a table that replaces the load+link. Next step.
2. **GX is still only a logging stub.** Next real work: command recording + TEV state -> shader.
3. **Audio**: `msm*` are stubs; `src/msm` (the real wrapper over MusyX) is not built yet.
4. **Float/double ABI**: Metrowerks passes float args as doubles. Any `extern` declaration that disagrees with the
   real definition (`HuSetVecF` in `mapspace.c` was one; patched) silently passes garbage on x86-64/PPC64.
5. **GXSetArray size**: PC GX API wants a byte size, game passes none (~160 call sites); shim passes 0.
6. **Pointers in u32**: ~470 pointer<->u32 casts. Host runs `-no-pie` and the arena is MAP_32BIT so they fit;
   on PS3 the arena printed at 0x5000xxxx (fits). Addresses >= 0x80000000 would sign-extend if kept in s32.
7. **7 REL files** use Metrowerks-only constructs: m404/m406/m413/m438/m447/w01/w06.
8. `OSPanic`: `fault.c` defines its own (draws to the XFB); the host one is weak and loses.
9. **PS3 backend of the fibers** (`fiber_os.c`, PORT_PS3 branch) is written from memory of PSL1GHT's thread/semaphore
   API and is untested. Each game process is a thread (256 KB stack) handing a baton to the next.
