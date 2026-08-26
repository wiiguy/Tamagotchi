#!/usr/bin/env bash
# Autonomous dev loop for the ORB firmware.
#
# Loop forever:
#   1. compile the sketch
#   2. if it compiled, flash it over /dev/ttyACM0
#   3. capture ~9s of boot serial output into orb/build/device.log
#   4. wait until a source file changes OR orb/.reflash is touched
#
# Re-flash without editing anything:   touch orb/.reflash
# Stop the loop:                       pkill -f devloop.sh
set -u
cd "$(dirname "$0")"

PORT="${PORT:-/dev/ttyACM0}"
FQBN='esp32:esp32:esp32c3:CDCOnBoot=cdc,FlashSize=4M'
BUILD=orb/build
STATE=.devstate
mkdir -p "$BUILD"
touch "$STATE"

cycle=0
while true; do
  cycle=$((cycle + 1))
  echo ""
  echo "======== cycle $cycle  $(date +%T) ========"
  rm -f "$BUILD/compile.log" "$BUILD/upload.log"

  if arduino-cli compile --fqbn "$FQBN" --build-path "$BUILD" orb \
      >"$BUILD/compile.log" 2>&1; then
    grep -E 'Sketch uses|Global variables' "$BUILD/compile.log" || true
    echo "--- flashing ---"
    if arduino-cli upload --fqbn "$FQBN" -p "$PORT" --input-dir "$BUILD" \
        >>"$BUILD/upload.log" 2>&1; then
      echo "--- flashed OK, capturing serial ---"
      : >"$BUILD/device.log"
      python3 tools/serial_sniff.py "$PORT" 9 2>&1 | tee "$BUILD/device.log"
    else
      echo "--- FLASH FAILED ---"
      tail -6 "$BUILD/upload.log"
    fi
  else
    echo "--- COMPILE FAILED ---"
    grep -m 6 'error:' "$BUILD/compile.log" || tail -6 "$BUILD/compile.log"
  fi

  # wait for a source change or an explicit trigger
  while true; do
    if [ -e orb/.reflash ]; then
      rm -f orb/.reflash
      break
    fi
    changed=$(find orb -name '*.ino' -o -name '*.h' 2>/dev/null | while read -r f; do
      [ "$f" -nt "$STATE" ] && echo "$f"
    done | head -1)
    if [ -n "$changed" ]; then
      touch "$STATE"
      break
    fi
    sleep 1
  done
done
