#!/bin/sh
# Merge one REL's objects into a single object and prefix every global symbol it defines, so ~95 overlays that
# all define _prolog/ObjectSetup/main-like names can live in one ELF.
#
# Exception: COMMON symbols that the non-REL (game/SDK/shim) objects also define are left alone. Headers such as
# dolphin/os.h contain tentative definitions (u32 __OSBusClock; ...) that every translation unit - REL ones too -
# turns into a common symbol. Renaming those would give each overlay a private zeroed copy of OS globals instead of
# the shared one the game initializes.
#
# usage: prefix_rel.sh <ld> <nm> <objcopy> <prefix> <base_syms.txt> <out.o> <objs...>
LD=$1; NM=$2; OBJCOPY=$3; PFX=$4; BASE=$5; OUT=$6; shift 6
"$LD" -r "$@" -o "$OUT.merged" || exit 1
# '.name' entry-point symbols (PPC64 ELFv1) need the dot kept in front
"$NM" -g --defined-only "$OUT.merged" | awk -v p="$PFX" '
    NR == FNR { base[$1] = 1; next }
    NF == 3 {
        s = $3
        if ($2 == "C" && (s in base)) next
        if (substr(s, 1, 1) == ".") print s, "." p substr(s, 2); else print s, p s
    }' "$BASE" - > "$OUT.syms"
"$OBJCOPY" --redefine-syms="$OUT.syms" "$OUT.merged" "$OUT" || exit 1
