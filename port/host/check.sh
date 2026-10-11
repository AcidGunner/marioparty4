#!/bin/sh
# Syntax-check game sources against host gcc using the TARGET_PC header paths.
# usage: port/host/check.sh [dir...]   (default: src/game)
cd "$(dirname "$0")/../.." || exit 1
DIRS=${@:-src/game}
FLAGS="-DTARGET_PC -DVERSION=1 -DMUSY_TARGET=MUSY_TARGET_PC -fcommon -fgnu89-inline -fno-pie -include port_compat.h -w -fsyntax-only -std=gnu11 -fms-extensions -Iport/host/include -idirafter include -Iextern/musyx/include -Ibuild/GMPE01_01/include"
ok=0; bad=0
for d in $DIRS; do
  for f in $(find $d -name '*.c' | sort); do
    if gcc $FLAGS $f 2>/tmp/err_$$.txt; then ok=$((ok+1)); else bad=$((bad+1)); echo "FAIL $f ($(grep -c 'error:' /tmp/err_$$.txt) errors)"; fi
  done
done
echo "ok=$ok fail=$bad"
