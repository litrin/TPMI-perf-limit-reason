#!/bin/sh
# Summarize plr-tpmi output: per-domain die-level and per-core reason histogram.
set -u

TOOL=${TOOL:-/tmp/plr-tpmi}

"$TOOL" > /tmp/plr.out 2>&1

echo "--- domains / die-level ---"
grep -E 'TPMI device|domain |die-level' /tmp/plr.out

echo
echo "--- per-core reason histogram (count reason...) ---"
grep -oE '0x[0-9a-f]{16} :.*' /tmp/plr.out | sed 's/^[^:]*: //' | sort | uniq -c

echo
echo "--- totals ---"
echo "cores reported : $(grep -c '^    module ' /tmp/plr.out)"
echo "output lines   : $(wc -l < /tmp/plr.out)"
