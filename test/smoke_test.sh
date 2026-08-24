#!/bin/sh
# Smoke test for plr-tpmi: clear status, apply an all-core load, re-read.
set -u

TOOL=${TOOL:-/tmp/plr-tpmi}
NCPU=$(getconf _NPROCESSORS_ONLN)
DUR=${DUR:-15}

echo "=== platform ==="
grep -m1 'model name' /proc/cpuinfo
echo "cpus: $NCPU"

echo
echo "=== TPMI features ==="
"$TOOL" -l | head -40

echo
echo "=== clear, then idle read ==="
"$TOOL" -c >/dev/null 2>&1
"$TOOL" | sed -n '1,10p'

echo
echo "=== applying all-core load for ${DUR}s ==="
i=0
while [ "$i" -lt "$NCPU" ]; do
	taskset -c "$i" sh -c "end=\$((\$(date +%s)+$DUR)); while [ \$(date +%s) -lt \$end ]; do :; done" &
	i=$((i + 1))
done

sleep $((DUR - 5))
echo "=== during load ==="
"$TOOL" | sed -n '1,14p'

wait
sleep 2
echo
echo "=== after load (latched bits) ==="
"$TOOL" | sed -n '1,10p'

echo
echo "=== unique reasons seen across all cores ==="
"$TOOL" | grep -oE '0x[0-9a-f]{16} :.*' | sed 's/^[^:]*: //' | sort -u
