#!/bin/bash
# Link a cross-emitted generated/ tree into a loadable module on the Pi.
#   usage: build_module.sh <arm-name> <game-id>
# Expects /home/recomp/pi4gap/<arm>/generated to already be in place.
set -eu
ARM=$1; GAME=$2
ROOT=/home/recomp/pi4gap/$ARM
TPL=/home/recomp/dev/ModernGekko/vendor/dolphin/module-template

test -d "$ROOT/generated" || { echo "no $ROOT/generated"; exit 1; }
test -f "$ROOT/generated/main.dol" || { echo "generated/main.dol missing"; exit 1; }

# The DOL that was recompiled must be the DOL the runtime loads. This is the
# check that was missing when Colosseum modules were built from a patched image
# and measured for a day before the 15 differing bytes were noticed.
WANT=$(sha256sum /home/recomp/dev/extracted/$GAME/sys/main.dol | cut -d' ' -f1)
GOT=$(sha256sum "$ROOT/generated/main.dol" | cut -d' ' -f1)
if [ "$WANT" != "$GOT" ]; then
  echo "REFUSING: recompiled DOL does not match the runtime's DOL"
  echo "  runtime:    $WANT"
  echo "  recompiled: $GOT"
  exit 1
fi
echo "  ok  DOL matches: $GOT"

cmake -S "$TPL" -B "$ROOT/build" \
  -DGAME_ID="$GAME" \
  -DGENERATED_DIR="$ROOT/generated" \
  -DRECOMPCORE_MODULE_ENABLE_IPO=ON \
  -DRECOMPCORE_MODULE_OPT_LEVEL=2 \
  -DCMAKE_BUILD_TYPE=Release > "$ROOT/cmake.log" 2>&1 || { tail -20 "$ROOT/cmake.log"; exit 1; }

cmake --build "$ROOT/build" -j4 > "$ROOT/build.log" 2>&1 || { tail -30 "$ROOT/build.log"; exit 1; }

SO="$ROOT/build/g${GAME}_recomp.so"
test -f "$SO" || { echo "no module produced"; exit 1; }
echo "  built $SO"
echo "  size  $(stat -c%s "$SO")"
echo "  sha   $(sha256sum "$SO" | cut -d' ' -f1)"
