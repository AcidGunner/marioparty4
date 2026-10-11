# Host (Linux, 64-bit gcc) bring-up build for the Mario Party 4 decomp.
# Run from repo root:  make -f port/host/Makefile [check|test|stubs|link]
#
# Skipped for now:
#   jmp.c      PPC32 asm context switch (needs fibers; see port/README.md)
#   kerent.c   Metrowerks asm jump table for REL imports (obsolete when overlays are static)
#   fault.c font.c hsfman.c sreset.c   #include .inc data extracted from the user's own disc
SKIP := jmp kerent
CFLAGS := -fcommon -fgnu89-inline -fno-pie -DTARGET_PC -DVERSION=1 -DMUSY_TARGET=MUSY_TARGET_PC -include port_compat.h \
          -w -g -std=gnu11 -fms-extensions -Iport/host/include -idirafter include -Iextern/musyx/include -Ibuild/GMPE01_01/include
OUT := build/host
GAME_SRC := $(filter-out $(addprefix src/game/,$(addsuffix .c,$(SKIP))),$(shell find src/game src/libhu -name '*.c' | sort))
GAME_OBJ := $(patsubst %.c,$(OUT)/%.o,$(GAME_SRC))
# Overlays whose SDK references should get stubs too (keep in sync with PORT_RELS in port/ps3/Makefile)
PORT_RELS ?= bootDll
REL_SRC := $(foreach r,$(PORT_RELS),$(wildcard src/REL/$(r)/*.c))
REL_OBJ := $(patsubst %.c,$(OUT)/%.o,$(REL_SRC))
SHIM_SRC := $(wildcard port/host/src/*.c)
SHIM_OBJ := $(patsubst %.c,$(OUT)/%.o,$(SHIM_SRC))

-include $(GAME_OBJ:.o=.d) $(SHIM_OBJ:.o=.d)

all: link

$(OUT)/%.o: %.c
	@mkdir -p $(dir $@)
	@gcc $(CFLAGS) -MD -MP -c $< -o $@

check:
	@port/host/check.sh src/game src/REL

# Regenerate logging stubs for every SDK symbol game code references but we don't implement.
stubs: $(GAME_OBJ) $(REL_OBJ)
	@nm -u $(GAME_OBJ) $(REL_OBJ) | awk '{print $$2}' | sort -u > $(OUT)/undef.txt
	@nm -g --defined-only $(GAME_OBJ) | awk 'NF==3{print $$3}' | sort -u > $(OUT)/def.txt
	@comm -23 $(OUT)/undef.txt $(OUT)/def.txt | grep -E '^(__)?(GX|OS|PAD|VI|DVD|AI|AR|ARQ|CARD|THP|DC|LC|SI|msm|DEMO|PPC)' > $(OUT)/sdk_symbols.txt
	python3 port/host/tools/gen_stubs.py . $(OUT)/sdk_symbols.txt port/host/src/sdk_host.c port/host/src/dvd_host.c port/host/src/aram_host.c port/host/src/gx_soft.c > port/host/src/sdk_stubs_generated.c

# Links with unresolved symbols tolerated so you can see how far it gets; the report lists what's missing.
link: $(GAME_OBJ) $(SHIM_OBJ)
	@gcc -no-pie $(GAME_OBJ) $(SHIM_OBJ) -lm -lpthread -Wl,--unresolved-symbols=ignore-all -o $(OUT)/mp4_host
	@echo "linked $(OUT)/mp4_host"
	@gcc -no-pie $(GAME_OBJ) $(SHIM_OBJ) -lm -lpthread -o /dev/null 2>&1 | grep -oE "undefined reference to .[^']+" | sed "s/undefined reference to .//" | sort -u > $(OUT)/unresolved.txt || true
	@echo "unresolved symbols: $$(wc -l < $(OUT)/unresolved.txt) (see $(OUT)/unresolved.txt)"

TESTCC = gcc $(CFLAGS) -no-pie
test: $(OUT)/mtx_test $(OUT)/fiber_test $(OUT)/dvd_test
	@$(OUT)/mtx_test
	@$(OUT)/fiber_test | tail -1
	@mkdir -p $(OUT)/fakedisc/Data && head -c 1000 /dev/urandom > $(OUT)/fakedisc/Data/Board.bin && head -c 77 /dev/urandom > $(OUT)/fakedisc/meminfo.bin
	@PORT_DISC_ROOT=$(OUT)/fakedisc $(OUT)/dvd_test 2>/dev/null

$(OUT)/mtx_test: port/host/tests/mtx_test.c port/host/src/mtx_host.c
	@mkdir -p $(OUT)
	@$(TESTCC) $^ -lm -o $@

$(OUT)/fiber_test: port/host/tests/fiber_test.c port/host/src/fiber.c port/host/src/fiber_os.c
	@mkdir -p $(OUT)
	@$(TESTCC) $^ -lpthread -o $@

$(OUT)/dvd_test: port/host/tests/dvd_test.c port/host/src/dvd_host.c
	@mkdir -p $(OUT)
	@$(TESTCC) $^ -o $@

clean:
	rm -rf $(OUT)
.PHONY: all check stubs link test clean
