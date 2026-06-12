#!/usr/bin/env bash
set -euo pipefail

IMAGE="vitaimmich-builder"
OUTPUT_DIR="$(cd "$(dirname "$0")" && pwd)/build"

mkdir -p "$OUTPUT_DIR"

echo "Building Docker image..."
docker build -t "$IMAGE" "$(dirname "$0")"

echo "Running build..."
docker run --rm \
    -v "$OUTPUT_DIR:/output" \
    "$IMAGE"

echo ""
echo "VPK ready: $OUTPUT_DIR/vitaImmich.vpk"
