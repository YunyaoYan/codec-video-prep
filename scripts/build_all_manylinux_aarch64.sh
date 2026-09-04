#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${AARCH64_WHEELHOUSE:-$ROOT/wheelhouse_aarch64}"
PY_TAGS="${PY_TAGS:-cp310-cp310 cp311-cp311 cp312-cp312 cp313-cp313}"
MANYLINUX_IMAGE="${MANYLINUX_IMAGE:-quay.io/pypa/manylinux2014_aarch64:latest}"
PY314_TAGS="${PY314_TAGS:-cp314-cp314}"
PY314_MANYLINUX_IMAGE="${PY314_MANYLINUX_IMAGE:-quay.io/pypa/manylinux_2_28_aarch64:latest}"
FFMPEG_BUILD_SCRIPT="${FFMPEG_BUILD_SCRIPT:-build_pixel_ffmpeg.sh}"

rm -rf "$OUT"
mkdir -p "$OUT"

first=1
for tag in $PY_TAGS; do
  echo "========================================"
  echo "Building aarch64 $tag"
  echo "FFmpeg build script: $FFMPEG_BUILD_SCRIPT"
  echo "========================================"
  if [[ "$first" == "1" ]]; then
    MANYLINUX_IMAGE="$MANYLINUX_IMAGE" FFMPEG_BUILD_SCRIPT="$FFMPEG_BUILD_SCRIPT" PY_TAG="$tag" bash "$ROOT/scripts/build_manylinux_wheel.sh"
    first=0
  else
    MANYLINUX_IMAGE="$MANYLINUX_IMAGE" FFMPEG_BUILD_SCRIPT="$FFMPEG_BUILD_SCRIPT" REUSE_FFMPEG=1 PY_TAG="$tag" bash "$ROOT/scripts/build_manylinux_wheel.sh"
  fi
  cp "$ROOT"/wheelhouse/*.whl "$OUT"/
done


for tag in $PY314_TAGS; do
  echo "========================================"
  echo "Building aarch64 $tag ($PY314_MANYLINUX_IMAGE)"
  echo "FFmpeg build script: $FFMPEG_BUILD_SCRIPT"
  echo "========================================"
  MANYLINUX_IMAGE="$PY314_MANYLINUX_IMAGE" \
    FFMPEG_BUILD_SCRIPT="$FFMPEG_BUILD_SCRIPT" \
    PY_TAG="$tag" \
    bash "$ROOT/scripts/build_manylinux_wheel.sh"
  cp "$ROOT"/wheelhouse/*.whl "$OUT"/
done

echo "All aarch64 wheels:"
ls -lh "$OUT"
