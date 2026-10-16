# Mario Party 4 decompilation - PSL1GHT / PS3 package build
# Requires PS3DEV, PSL1GHT and PORTLIBS to be set up for your ps3toolchain.
# This is an initial integration; packaging may need small adjustments for the
# exact ppu_rules / package_gen version installed in your toolchain.

.SUFFIXES:

ifeq ($(strip $(PSL1GHT)),)
	$(error "PSL1GHT is not set (source your ps3toolchain environment)")
endif

TITLE		:= Mario Party 4
APPID		:= MP4PORT01
CONTENTID	:= HP0031-$(APPID)_00-0000000000000000
ICON0		?= port/ps3/ICON0.PNG

# ignore this
BUILD		:= build/ps3
BUILDDIR	:= $(CURDIR)/build/ps3

include $(PSL1GHT)/ppu_rules

TARGET		:= marioparty4
VERSION_NO	:= 1
OUT			:= build/ps3
OUTPUT		:= $(CURDIR)/$(TARGET)

SKIP		:= jmp kerent

# Overlays linked statically into the ELF. Each name is a directory in src/REL (case as on disk).
# Start with bootDll; add more once the previous ones run (RELs that use board_executor.c need a variant of rel_executor.c).
PORT_RELS	?= bootDll modeseldll

lc = $(shell echo $(1) | tr 'A-Z' 'a-z')
REL_BIN		:= $(PS3DEV)/ppu/bin/ppu-
REL_LD		:= $(REL_BIN)ld
REL_NM		:= $(REL_BIN)nm
REL_OBJCOPY := $(REL_BIN)objcopy

REL_OBJS	:= $(foreach r,$(PORT_RELS),$(OUT)/rel/$(r).o)
GAME_SRC	:= $(filter-out $(addprefix src/game/,$(addsuffix .c,$(SKIP))),$(shell find src/game src/libhu -name '*.c' | sort))
SHIM_SRC	:= $(wildcard port/host/src/*.c) $(wildcard port/ps3/src/*.c)
BASE_OBJS	:= $(patsubst %.c,$(OUT)/%.o,$(GAME_SRC) $(SHIM_SRC))
OBJS		:= $(BASE_OBJS) $(REL_OBJS)

CFLAGS		:= -O2 -g -mcpu=cell $(MACHDEP) -std=gnu11 -fms-extensions \
				-fgnu89-inline -fcommon -w \
				-DTARGET_PC -DPORT_PS3 -DVERSION=$(VERSION_NO) \
				-DMUSY_TARGET=MUSY_TARGET_PC \
				-include port_compat.h \
				-I$(OUT) -Iport/host/include -idirafter include \
				-Iextern/musyx/include -Ibuild/GMPE01_01/include \
				-I$(PORTLIBS)/include -I$(PSL1GHT)/ppu/include \
				-I$(PS3DEV)/ppu/include

LDFLAGS		:= $(MACHDEP) -Wl,-Map,$(OUT)/$(TARGET).map \
				-L$(PSL1GHT)/ppu/lib -L$(PORTLIBS)/lib
LIBS		:= -lrsx -lgcm_sys -lio -lsysutil -lrt -llv2 -lm

SCETOOL_FLAGS +=

-include $(OBJS:.o=.d)

.PHONY: all elf self pkg clean errors
all: elf
elf: $(OUT)/$(TARGET).elf

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

$(OUT)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo "CC $<"
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(OUT)/$(TARGET).elf: $(OBJS)
	@mkdir -p $(OUT)
	@echo "LINK $@"
	@$(CC) $(OBJS) $(LDFLAGS) $(LIBS) -o $@

errors:
	@-$(MAKE) -k -f port/ps3/Makefile all > $(OUT)/build.log 2>&1; grep -E "error|undefined reference" $(OUT)/build.log | sort | uniq -c | sort -rn | head -60

self: $(OUT)/$(TARGET).self
$(OUT)/%.self: $(OUT)/%.elf
	@$(SPRX) $< && $(STRIP) $< -o $(OUT)/$*_stripped.elf && $(SELF) $(OUT)/$*_stripped.elf $@

pkg: $(OUT)/$(TARGET).pkg

clean:
	@echo cleaning ...
	@rm -rf $(OUT)

