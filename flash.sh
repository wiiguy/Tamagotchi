#!/usr/bin/env bash
# One-shot: compile ORB, flash it to /dev/ttyACM0, print 10s of serial log.
# Needs raw USB-serial access, so run it outside a restricted sandbox:
#   ./flash.sh
set -u
cd "$(dirname "$0")"

PORT="${PORT:-/dev/ttyACM0}"
FQBN='esp32:esp32:esp32c3:CDCOnBoot=cdc,FlashSize=4M'
BUILD=orb/build
LOG="$BUILD/device.log"
mkdir -p "$BUILD"

echo "== compile =="
if ! arduino-cli compile --fqbn "$FQBN" --build-path "$BUILD" orb 2>&1 | tail -3; then
  echo "== COMPILE FAILED =="
  grep -m 10 'error:' "$BUILD/compile.log" 2>/dev/null
  exit 1
fi

echo "== flash -> $PORT =="
if ! arduino-cli upload --fqbn "$FQBN" -p "$PORT" --input-dir "$BUILD"; then
  echo "== FLASH FAILED =="
  exit 1
fi

echo "== device serial (10s) =="
python3 tools/serial_sniff.py "$PORT" 10 | tee "$LOG"
echo "== done =="
