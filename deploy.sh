#!/bin/bash
set -euo pipefail

TITLE="VIMM00001"

cd "$(dirname "$0")"

pkill -9 -f vita3k 2>/dev/null || true
sleep 1

# Install
vita3k ./build/vitaImmich.vpk >/tmp/vita3k-install.log 2>&1 &
sleep 4

pkill -9 -f vita3k 2>/dev/null || true
sleep 1

# Launch detached
nohup vita3k -r "$TITLE" \
  >/tmp/vita3k-runtime.log 2>&1 \
  </dev/null &

PID=$!

echo "{\"status\":\"live\",\"pid\":$PID,\"title\":\"$TITLE\"}"
