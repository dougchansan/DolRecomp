#!/bin/bash
# Where the time actually goes, with the module running.
#   usage: profile_run.sh <module.so> <title> <cputhread> <out-prefix>
#
# Answers two questions the FPS number cannot:
#   1. is the generated module executing at all, and for what share of samples
#   2. what is the instruction/cycle/miss profile of the run
#
# perf_event_paranoid is 2 on this Pi, so this is userspace-only. That is
# enough to separate the generated module from the JIT from the runtime, which
# is the split that matters here.
set -u
MOD=$1; TITLE=$2; CPUTHREAD=$3; OUT=$4

export XDG_RUNTIME_DIR=/run/user/1001 WAYLAND_DISPLAY=wayland-1
export SWAYSOCK=$(ls /run/user/1001/sway-ipc*.sock 2>/dev/null | head -1)
export MODERNGEKKO_UBERSHADERS=0
export VK_ICD_FILENAMES=/home/recomp/mesa-252/share/vulkan/icd.d/broadcom_icd.aarch64.json

U=$HOME/.local/share/mariokart-doubledash-recomp
D=$U/Config/Dolphin.ini
RUN=/home/recomp/dev/ModernGekko/build-arm64/mgnew
GAME=/home/recomp/dev/extracted/$TITLE

LOCKFILE=/tmp/dolbench.lock
exec 9>"$LOCKFILE"
flock -n 9 || { echo "REFUSED: benchmark lock held"; exit 1; }

bash /home/recomp/preflight.sh "$MOD" || exit 1
pkill -9 -x mgnew 2>/dev/null; sleep 3

if grep -q '^CPUThread' "$D"; then sed -i "s|^CPUThread.*|CPUThread = $CPUTHREAD|" "$D"
else sed -i "/^\[Core\]/a CPUThread = $CPUTHREAD" "$D"; fi

cd /home/recomp/dev/ModernGekko/build-arm64 || exit 1
setsid --fork stdbuf -oL -eL "$RUN" --game "$GAME" --module "$MOD" \
  --user-dir "$U" --wayland > "$OUT.run.log" 2>&1 < /dev/null
sleep 12
PID=$(pgrep -x mgnew | head -1)
[ -n "$PID" ] || { echo "emulator did not start"; exit 1; }

# Settle into the measured scene before recording, so the profile is of the
# scene and not of loading.
if [ "$TITLE" = "GC6E01" ]; then sleep 150; else sleep 120; fi

echo "=== perf stat (30s, whole process) ==="
perf stat -p "$PID" -e instructions,cycles,branch-misses,cache-misses \
  -- sleep 30 2>&1 | tee "$OUT.stat.txt" | tail -14

echo "=== perf record (30s, call-graph off for lower overhead) ==="
perf record -p "$PID" -F 499 -o "$OUT.data" -- sleep 30 > /dev/null 2>&1

pkill -TERM -x mgnew 2>/dev/null
for i in $(seq 1 20); do grep -q 'shutdown:' "$OUT.run.log" && break; sleep 1; done
grep 'shutdown:' "$OUT.run.log" | tail -1 | tee "$OUT.shutdown.txt"
pkill -9 -x mgnew 2>/dev/null

echo "=== where the samples landed, by object ==="
perf report -i "$OUT.data" --sort dso --stdio 2>/dev/null | head -25 | tee "$OUT.dso.txt"

echo "=== hottest symbols ==="
perf report -i "$OUT.data" --sort symbol --stdio 2>/dev/null | head -35 | tee "$OUT.sym.txt"

sed -i 's|^CPUThread.*|CPUThread = False|' "$D"
