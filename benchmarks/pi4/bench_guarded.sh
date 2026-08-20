#!/bin/bash
# Paired A/B benchmark with the guards that previous rounds of this work needed
# and did not have. Emits one JSON object per run to <out>.jsonl so a result can
# be re-checked later without trusting a summary line.
#
# usage: bench_guarded.sh <A.so> <A-label> <B.so> <B-label> <out-prefix> <title> <cputhread> [pairs]
#   title: GC6E01 (Colosseum, self-driving intro) or GLME01 (Luigi's Mansion, pad-driven)
#
# Each guard below exists because its absence produced a number that had to be
# withdrawn:
#   - a second emulator halved the framerate and the title scrape read either one
#   - Dolphin rewrote CPUThread on exit and every later run inherited it
#   - a black screen reported a plausible FPS for an hour
#   - the object cache served the previous emitter's objects
#   - the module was not executing at all
# Thermal state is new here: this Pi reports throttled=0x50000, meaning
# under-voltage and throttling have both occurred since boot, so a run that
# throttles has to be visible rather than averaged in.
set -u

A=$1; A_LABEL=$2; B=$3; B_LABEL=$4; OUT=$5; TITLE=$6; CPUTHREAD=$7; PAIRS=${8:-5}
JSONL="${OUT}.jsonl"
LOG="${OUT}.txt"

export XDG_RUNTIME_DIR=/run/user/1001 WAYLAND_DISPLAY=wayland-1
export SWAYSOCK=$(ls /run/user/1001/sway-ipc*.sock 2>/dev/null | head -1)
export MODERNGEKKO_UBERSHADERS=0
export VK_ICD_FILENAMES=/home/recomp/mesa-252/share/vulkan/icd.d/broadcom_icd.aarch64.json

U=$HOME/.local/share/mariokart-doubledash-recomp
D=$U/Config/Dolphin.ini
GFX=$U/Config/GFX.ini
RUN=/home/recomp/dev/ModernGekko/build-arm64/mgnew
GAME=/home/recomp/dev/extracted/$TITLE
DOL=$GAME/sys/main.dol

# ---------------------------------------------------------------- exclusivity
# A lock, not a pgrep. pgrep races: two scripts can both see zero and both
# start. flock cannot.
LOCKFILE=/tmp/dolbench.lock
exec 9>"$LOCKFILE"
if ! flock -n 9; then
  echo "REFUSED: another benchmark holds $LOCKFILE (pid $(cat $LOCKFILE 2>/dev/null))"
  exit 1
fi
echo $$ >&9

bash /home/recomp/preflight.sh "$A" "$B" || exit 1

: > "$LOG"; : > "$JSONL"

# ------------------------------------------------------------------ provenance
# Recorded once, per file, so a result can be tied to exactly what ran.
sha() { sha256sum "$1" 2>/dev/null | cut -d' ' -f1; }
DOL_SHA=$(sha "$DOL"); RUN_SHA=$(sha "$RUN")
A_SHA=$(sha "$A"); B_SHA=$(sha "$B")
KERNEL=$(uname -r)
{
  echo "title       $TITLE"
  echo "dol         $DOL_SHA"
  echo "runtime     $RUN_SHA"
  echo "A $A_LABEL  $A_SHA  $A"
  echo "B $B_LABEL  $B_SHA  $B"
  echo "cputhread   requested=$CPUTHREAD"
  echo "kernel      $KERNEL"
} | tee -a "$LOG"

# ----------------------------------------------------------------- config
# Written from scratch every run rather than inherited. Dolphin rewrites this
# file when it exits, so anything not re-asserted is whatever the last run left.
apply_config() {
  local ct=$1
  python3 - "$D" "$ct" <<'PYEOF'
import sys, re, io
path, ct = sys.argv[1], sys.argv[2]
want = {"Core": {"CPUThread": ct, "GFXBackend": "Vulkan", "DSPHLE": "True"},
        "DSP":  {"Backend": "Pulse"}}
try:    text = io.open(path, encoding="utf-8", errors="replace").read()
except FileNotFoundError: text = ""
lines = text.splitlines()
out, section, seen = [], None, {}
for line in lines:
    m = re.match(r"\s*\[(.+?)\]\s*$", line)
    if m:
        section = m.group(1); out.append(line); continue
    m = re.match(r"\s*([A-Za-z0-9_]+)\s*=", line)
    if m and section in want and m.group(1) in want[section]:
        key = m.group(1)
        out.append("%s = %s" % (key, want[section][key]))
        seen.setdefault(section, set()).add(key)
        continue
    out.append(line)
for section, kv in want.items():
    have = seen.get(section, set())
    missing = [k for k in kv if k not in have]
    if not missing: continue
    try:    idx = out.index("[%s]" % section)
    except ValueError:
        out.append("[%s]" % section); idx = len(out) - 1
    for k in missing:
        out.insert(idx + 1, "%s = %s" % (k, kv[k]))
io.open(path, "w", encoding="utf-8").write("\n".join(out) + "\n")
PYEOF
}

# Read back what the file actually says, rather than trusting the edit.
effective() { grep -m1 "^$1" "$D" 2>/dev/null | sed 's/.*= *//' | tr -d ' \r'; }

# ------------------------------------------------------------------- thermal
therm_temp()  { vcgencmd measure_temp 2>/dev/null | grep -oE '[0-9.]+'; }
therm_clock() { vcgencmd measure_clock arm 2>/dev/null | cut -d= -f2; }
therm_state() { vcgencmd get_throttled 2>/dev/null | cut -d= -f2; }

restore() {
  cp "$U/Config/GCPadNew.ini.orig" "$U/Config/GCPadNew.ini" 2>/dev/null
  cp "$U/config.ini.orig" "$U/config.ini" 2>/dev/null
  apply_config False
  echo "config-restored" | tee -a "$LOG"
}
trap restore EXIT

if [ "$TITLE" = "GLME01" ]; then
  sed -i 's|Device = SDL/0/Nintendo Switch PowerA Controller|Device = SDL/0/DolBench Pad|' "$U/Config/GCPadNew.ini"
  sed -i 's|controller1=SDL/0/Nintendo Switch PowerA Controller|controller1=SDL/0/DolBench Pad|' "$U/config.ini"
fi

kill_all() {
  pkill -9 -x mgnew 2>/dev/null
  pkill -9 -x moderngekko-run 2>/dev/null
  pkill -9 -f '[d]rive_loop.py' 2>/dev/null
  sleep 3
}

emit_json() {
  python3 -c '
import json,sys
d=dict(a.split("=",1) for a in sys.argv[1:] if "=" in a)
for k in ("mean","sd","min","max","temp_before","temp_after"):
    if k in d:
        try: d[k]=float(d[k])
        except ValueError: pass
for k in ("n","cycles","png","pairs_index"):
    if k in d:
        try: d[k]=int(d[k])
        except ValueError: pass
d["samples"]=[float(x) for x in d.get("samples","").split(",") if x]
print(json.dumps(d))' "$@" >> "$JSONL"
}

run_one() {
  local MOD=$1 LABEL=$2 MODSHA=$3 IDX=$4 RUNLOG=/tmp/bg-$2.log
  kill_all
  apply_config "$CPUTHREAD"

  local T0 C0 S0; T0=$(therm_temp); C0=$(therm_clock); S0=$(therm_state)

  if [ "$TITLE" = "GLME01" ]; then
    rm -f /tmp/foyer_ready
    setsid --fork python3 $HOME/drive_loop.py 320 > /dev/null 2>&1 < /dev/null
    sleep 3
  fi
  cd /home/recomp/dev/ModernGekko/build-arm64 || return 1
  setsid --fork stdbuf -oL -eL "$RUN" --game "$GAME" \
    --module "$MOD" --user-dir "$U" --wayland > "$RUNLOG" 2>&1 < /dev/null
  sleep 12
  if ! pgrep -x mgnew >/dev/null; then
    echo "$LABEL DISCARD emulator-exited" | tee -a "$LOG"
    emit_json label="$LABEL" verdict=discard reason=emulator-exited module="$MOD" module_sha="$MODSHA"
    return 1
  fi

  # Only one instance may be running. Checked after launch, not only before:
  # sway has autostarted a second emulator behind a benchmark before.
  local INSTANCES; INSTANCES=$(pgrep -c -x mgnew 2>/dev/null || echo 0)
  if [ "$INSTANCES" != "1" ]; then
    echo "$LABEL DISCARD instances=$INSTANCES" | tee -a "$LOG"
    emit_json label="$LABEL" verdict=discard reason=multiple-instances instances="$INSTANCES"
    kill_all; return 1
  fi

  if [ "$TITLE" = "GLME01" ]; then
    local W=0
    while [ ! -f /tmp/foyer_ready ] && [ $W -lt 300 ]; do sleep 3; W=$((W + 3)); done
    if [ ! -f /tmp/foyer_ready ]; then
      echo "$LABEL DISCARD never-reached-gameplay" | tee -a "$LOG"
      emit_json label="$LABEL" verdict=discard reason=never-reached-gameplay
      kill_all; return 1
    fi
    sleep 10
  else
    sleep 150
  fi

  local F='' i v
  for i in $(seq 1 20); do
    v=$(swaymsg -t get_tree 2>/dev/null | grep -oE '\| [0-9.]+ FPS' | head -1 | grep -oE '[0-9.]+')
    [ -n "$v" ] && F="$F $v"
    sleep 3
  done

  grim /tmp/bg-$2-a.png 2>/dev/null; sleep 3; grim /tmp/bg-$2-b.png 2>/dev/null
  local SZ P Q; SZ=$(stat -c%s /tmp/bg-$2-a.png 2>/dev/null || echo 0)
  P=$(md5sum /tmp/bg-$2-a.png 2>/dev/null | cut -d' ' -f1)
  Q=$(md5sum /tmp/bg-$2-b.png 2>/dev/null | cut -d' ' -f1)

  local CT_EFF GFX_EFF DSP_EFF
  CT_EFF=$(effective CPUThread); GFX_EFF=$(effective GFXBackend); DSP_EFF=$(effective Backend)

  local T1 C1 S1; T1=$(therm_temp); C1=$(therm_clock); S1=$(therm_state)

  pkill -TERM -x mgnew 2>/dev/null
  for i in $(seq 1 15); do grep -q 'shutdown:' "$RUNLOG" && break; sleep 1; done
  local CYC; CYC=$(grep 'shutdown:' "$RUNLOG" | tail -1 | grep -oE 'cycles=[0-9]+' | cut -d= -f2)
  kill_all

  local SAMPLES; SAMPLES=$(echo "$F" | tr -s ' ' | sed 's/^ //;s/ /,/g')
  local STATS; STATS=$(echo "$F" | awk '{s=0;k=0;mn=1e9;mx=0
     for(i=1;i<=NF;i++){s+=$i;k++;if($i<mn)mn=$i;if($i>mx)mx=$i}
     if(!k){print "0 99 0 0 0";exit}
     m=s/k;v=0;for(i=1;i<=NF;i++)v+=($i-m)*($i-m)
     printf "%.3f %.3f %.1f %.1f %d", m, sqrt(v/k), mn, mx, k}')
  local MEAN SD MN MX N
  read -r MEAN SD MN MX N <<< "$STATS"

  # ---- predeclared exclusion rules. Every discard is written to the JSONL
  # with its reason and its raw samples, so an excluded run is still evidence.
  local VERDICT=ok REASON=""
  if [ "${CYC:-0}" -lt 1000000000 ] 2>/dev/null; then VERDICT=discard; REASON=module-not-executing
  elif [ "$P" = "$Q" ];            then VERDICT=discard; REASON=frames-static
  elif [ "$SZ" -lt 200000 ];       then VERDICT=discard; REASON=not-rendering
  elif [ "$CT_EFF" != "$CPUTHREAD" ]; then VERDICT=discard; REASON=cputhread-drift
  elif [ "$S0" != "$S1" ];         then VERDICT=discard; REASON=throttled-during-run
  elif [ "$(awk -v d="$SD" 'BEGIN{print (d>0.6)?1:0}')" = "1" ]; then VERDICT=discard; REASON=unstable-scene
  fi
  if [ "$TITLE" = "GLME01" ] && [ "$VERDICT" = ok ]; then
    if [ "$(awk -v m="$MX" 'BEGIN{print (m>45)?1:0}')" = "1" ]; then VERDICT=discard; REASON=left-gameplay; fi
    if [ "$SZ" -lt 900000 ]; then VERDICT=discard; REASON=wrong-scene; fi
  fi

  emit_json label="$LABEL" verdict="$VERDICT" reason="$REASON" module="$MOD" \
    module_sha="$MODSHA" dol_sha="$DOL_SHA" runtime_sha="$RUN_SHA" title="$TITLE" \
    mean="$MEAN" sd="$SD" min="$MN" max="$MX" n="$N" samples="$SAMPLES" \
    cycles="${CYC:-0}" png="$SZ" frame_a="$P" frame_b="$Q" \
    cputhread="$CT_EFF" gfx="$GFX_EFF" dsp="$DSP_EFF" \
    temp_before="$T0" temp_after="$T1" clock_before="$C0" clock_after="$C1" \
    throttled_before="$S0" throttled_after="$S1" kernel="$KERNEL" pairs_index="$IDX"

  printf "%-18s %-8s mean=%-7s sd=%-6s n=%-3s cyc=%-12s png=%-8s ct=%-6s %s %s\n" \
    "$LABEL" "$VERDICT" "$MEAN" "$SD" "$N" "${CYC:-0}" "$SZ" "$CT_EFF" \
    "${T0}C->${T1}C" "$REASON" | tee -a "$LOG"
  [ "$VERDICT" = ok ]
}

for r in $(seq 1 "$PAIRS"); do
  # Alternate which arm runs first, so a warm-up or drift effect cannot
  # accumulate onto one arm.
  if [ $((r % 2)) -eq 1 ]; then
    run_one "$A" "$A_LABEL-r$r" "$A_SHA" "$r"
    run_one "$B" "$B_LABEL-r$r" "$B_SHA" "$r"
  else
    run_one "$B" "$B_LABEL-r$r" "$B_SHA" "$r"
    run_one "$A" "$A_LABEL-r$r" "$A_SHA" "$r"
  fi
done

echo "=== raw results: $JSONL ===" | tee -a "$LOG"
python3 - "$JSONL" "$A_LABEL" "$B_LABEL" <<'PYEOF' | tee -a "$LOG"
import json, sys, statistics
rows = [json.loads(l) for l in open(sys.argv[1]) if l.strip()]
a_label, b_label = sys.argv[2], sys.argv[3]
ok = [r for r in rows if r.get("verdict") == "ok"]
bad = [r for r in rows if r.get("verdict") != "ok"]
def arm(rs, lab): return [r for r in rs if r["label"].startswith(lab + "-r")]
pairs, wins_a, wins_b = [], 0, 0
idx = sorted({r.get("pairs_index") for r in ok})
for i in idx:
    a = [r for r in arm(ok, a_label) if r.get("pairs_index") == i]
    b = [r for r in arm(ok, b_label) if r.get("pairs_index") == i]
    if not a or not b: continue
    pairs.append((a[0]["mean"], b[0]["mean"]))
    if b[0]["mean"] > a[0]["mean"]: wins_b += 1
    else: wins_a += 1
print("complete pairs: %d   discarded runs: %d" % (len(pairs), len(bad)))
for r in bad:
    print("  discarded %-18s %s" % (r.get("label"), r.get("reason")))
if pairs:
    am = statistics.mean(x for x, _ in pairs)
    bm = statistics.mean(y for _, y in pairs)
    print("%-14s mean %.3f" % (a_label, am))
    print("%-14s mean %.3f" % (b_label, bm))
    print("delta %+.2f%%   pairs favouring %s: %d   favouring %s: %d"
          % (100.0 * (bm - am) / am, b_label, wins_b, a_label, wins_a))
    print("per-pair: " + "  ".join("%.2f->%.2f" % p for p in pairs))
PYEOF
