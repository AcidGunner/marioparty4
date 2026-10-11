# PS3 build (compile-first)

Status: **untested** (written without a PS3 toolchain). The aim of the first round is only to get a clean compile
and link, then iterate on the error output.

    export PS3DEV=...  PSL1GHT=...  PORTLIBS=$PS3DEV/portlibs/ppu   # your ps3toolchain env
    make -f port/ps3/Makefile errors     # builds with -k, prints a deduplicated error summary
    make -f port/ps3/Makefile            # -> build/ps3/mp4.elf
    make -f port/ps3/Makefile self       # -> build/ps3/mp4.self (RPCS3 / CFW)

Prerequisite: `build/GMPE01_01/include/*.inc` from your own disc (the decomp's dtk build step).

What differs from the host build (all behind `#ifdef PORT_PS3` in `port/host/src/sdk_host.c`):
arena via malloc (no mmap), `gettimeofday`/`usleep` for time and vsync. At boot it prints `sizeof(void*)` and the
arena address, which tells us whether the u32-stored-pointer problem is real on PS3.

Expected first problems (guesses): missing/different libc headers or functions (`MSL_C`-style names in
`include/*.h` shadowing newlib: the build uses `-idirafter include` to avoid this), `OSPanic`/`fault.c` conflicts,
64-bit pointer casts, anything calling `printf` variants.

Not done yet, so the elf will not get past the first game process: fibers (`gcsetjmp`/`gclongjmp` abort), file access
(DVD stubs), RSX graphics (GX is a logging stub), audio, input.
