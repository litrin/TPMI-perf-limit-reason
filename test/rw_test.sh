#!/bin/sh
# Exercise the write paths (--clear) and the sampling loop (--watch).
set -u

TOOL=${TOOL:-/tmp/plr-tpmi}

echo "--- before clear ---"
"$TOOL" | grep -m3 -E 'die-level|0x[0-9a-f]{16}'

echo
echo "--- clear ---"
"$TOOL" -c > /dev/null || echo "clear returned $?"
"$TOOL" | grep -m3 -E 'die-level|0x[0-9a-f]{16}'

echo
echo "--- watch: 3 samples @0.5s, active cores only ---"
"$TOOL" -a -w 0.5 -n 3 | grep -cE '0x[0-9a-f]{16}'
echo "watch exit: $?"

echo
echo "--- AVX-heavy load, look for FREQUENCY_CDYN* fine reasons ---"
NCPU=$(getconf _NPROCESSORS_ONLN)
i=0
while [ "$i" -lt "$NCPU" ]; do
	taskset -c "$i" openssl speed -evp aes-256-gcm -seconds 8 >/dev/null 2>&1 &
	i=$((i + 1))
done
sleep 6
"$TOOL" | grep -oE '0x[0-9a-f]{16} :.*' | sed 's/^[^:]*: //' | sort | uniq -c
wait
