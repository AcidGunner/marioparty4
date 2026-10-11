# PS3 (PSL1GHT / ps3toolchain) build of the Mario Party 4 decomp. UNTESTED: written without ppu-gcc.
# Run from the repo root:  make -f port/ps3/Makefile          (builds build/ps3/mp4.elf)
#                          make -f port/ps3/Makefile self     (signs to .self for RPCS3 / CFW)
# Needs PS3DEV and PSL1GHT set (ps3toolchain), and build/GMPE01_01/include/*.inc from your own disc.

ifeq ($(strip $(PSL1GHT)),)
$(error "PSL1GHT is not set (source your ps3toolchain env)")
endif
include $(PSL1GHT)/ppu_rules

TARGET   := mp4
OUT      := build/ps3
VERSION_NO := 1

SKIP     := jmp kerent

# Overlays linked statically into the ELF. Each name is a directory in src/REL (case as on disk).
# Start with bootDll; add more once the previous ones run (RELs that use board_executor.c need a variant of rel_executor.c).
PORT_RELS ?= bootDll
lc = $(shell echo $(1) | tr 'A-Z' 'a-z')
REL_BIN     := $(PS3DEV)/ppu/bin/ppu-
REL_LD      := $(REL_BIN)ld
REL_NM      := $(REL_BIN)nm
REL_OBJCOPY := $(REL_BIN)objcopy
REL_OBJS    := $(foreach r,$(PORT_RELS),$(OUT)/rel/$(r).o)
GAME_SRC := $(filter-out $(addprefix src/game/,$(addsuffix .c,$(SKIP))),$(shell find src/game src/libhu -name '*.c' | sort))
SHIM_SRC := $(wildcard port/host/src/*.c) $(wildcard port/ps3/src/*.c)
OBJS     := $(patsubst %.c,$(OUT)/%.o,$(GAME_SRC) $(SHIM_SRC)) $(REL_OBJS)

# TARGET_PC selects the repo's PC-flavoured GX/type headers; PORT_PS3 selects PS3 branches in the shims.
CFLAGS   := -O2 -g -mcpu=cell $(MACHDEP) -std=gnu11 -fms-extensions -fgnu89-inline -fcommon -w \
            -DTARGET_PC -DPORT_PS3 -DVERSION=$(VERSION_NO) -DMUSY_TARGET=MUSY_TARGET_PC \
            -include port_compat.h \
            -I$(OUT) -Iport/host/include -idirafter include -Iextern/musyx/include -Ibuild/GMPE01_01/include \
            -I$(PORTLIBS)/include -I$(PSL1GHT)/ppu/include -I$(PS3DEV)/ppu/include
LDFLAGS  := $(MACHDEP) -Wl,-Map,$(OUT)/$(TARGET).map -L$(PSL1GHT)/ppu/lib -L$(PORTLIBS)/lib
LIBS     := -lrsx -lgcm_sys -lio -lsysutil -lrt -llv2 -lm

-include $(OBJS:.o=.d)

# rel_list.h: one REL(name) line per overlay, consumed by port/ps3/src/rel_table.c
$(OUT)/rel_list.h: port/ps3/Makefile
	@mkdir -p $(OUT)
	@printf '$(foreach r,$(PORT_RELS),REL($(call lc,$(r)))\n)' > $@
$(OUT)/port/ps3/src/rel_table.o: $(OUT)/rel_list.h

# One merged, symbol-prefixed object per overlay (see port/rel/prefix_rel.sh)
# Names defined by the game/SDK/shim objects (not the overlays): shared commons must not be prefixed per overlay
$(OUT)/base_syms.txt: $(patsubst %.c,$(OUT)/%.o,$(GAME_SRC) $(SHIM_SRC))
	@mkdir -p $(OUT)
	@$(REL_NM) -g --defined-only $^ | awk 'NF==3 {print $$3}' | sort -u > $@

define REL_template
$(OUT)/rel/$(1).o: $(patsubst %.c,$(OUT)/%.o,$(wildcard src/REL/$(1)/*.c)) $(OUT)/port/rel/rel_executor.o $(OUT)/base_syms.txt
	@mkdir -p $$(dir $$@)
	@echo rel $(1)
	@sh port/rel/prefix_rel.sh $(REL_LD) $(REL_NM) $(REL_OBJCOPY) rel_$(call lc,$(1))_ $(OUT)/base_syms.txt $$@ $$(filter %.o,$$^)
endef
$(foreach r,$(PORT_RELS),$(eval $(call REL_template,$(r))))

all: $(OUT)/$(TARGET).elf

$(OUT)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo $<
	@$(CC) $(CFLAGS) -MD -MP -c $< -o $@

$(OUT)/$(TARGET).elf: $(OBJS)
	@echo linking $@
	@$(CC) $(OBJS) $(LDFLAGS) $(LIBS) -o $@

# Compile everything, keep going after errors, and write all errors to one file you can paste back.
errors:
	@-$(MAKE) -k -f port/ps3/Makefile all > $(OUT)/build.log 2>&1; grep -E "error|undefined reference" $(OUT)/build.log | sort | uniq -c | sort -rn | head -60

self: $(OUT)/$(TARGET).self
$(OUT)/%.self: $(OUT)/%.elf
	@$(SPRX) $< && $(STRIP) $< -o $(OUT)/$*_stripped.elf && $(SELF) $(OUT)/$*_stripped.elf $@

clean:
	rm -rf $(OUT)
.PHONY: all errors self clean
