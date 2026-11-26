#!/bin/zsh
# Usage: ./flash.sh <sketch folder> [seconds to show serial output, default 8]
# Compiles and uploads over the USB port, then prints the serial output. No button needed once firmware with USB CDC is on the board.
set -e
cd "$(dirname "$0")"
SKETCH="${1:?sketch folder name}"
SECS="${2:-8}"
FQBN="esp32:esp32:esp32s3:CDCOnBoot=cdc"
PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
[ -n "$PORT" ] || { echo "No ESP on USB. Plug in, or hold BOOT while pressing RESET once."; exit 1; }
arduino-cli compile --fqbn "$FQBN" "$SKETCH" 2>&1 | tail -3
arduino-cli upload --fqbn "$FQBN" -p "$PORT" "$SKETCH" 2>&1 | tail -4
sleep 2
PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
[ -n "$PORT" ] || { echo "Port did not come back after upload."; exit 1; }
stty -f "$PORT" 115200
cat "$PORT" &
PID=$!
sleep "$SECS"
kill $PID 2>/dev/null || true
