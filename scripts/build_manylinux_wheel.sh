#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="${MANYLINUX_IMAGE:-quay.io/pypa/manylinux2014_x86_64:latest}"
PY_TAG="${PY_TAG:-cp310-cp310}"
PIP_INDEX_URL="${PIP_INDEX_URL:-}"
PIP_TRUSTED_HOST="${PIP_TRUSTED_HOST:-}"
REUSE_FFMPEG="${REUSE_FFMPEG:-0}"

DOCKER_ENV=()
if [[ -n "$PIP_INDEX_URL" ]]; then
  DOCKER_ENV+=("-e" "PIP_INDEX_URL=$PIP_INDEX_URL")
fi
if [[ -n "$PIP_TRUSTED_HOST" ]]; then
  DOCKER_ENV+=("-e" "PIP_TRUSTED_HOST=$PIP_TRUSTED_HOST")
fi
DOCKER_ENV+=("-e" "REUSE_FFMPEG=$REUSE_FFMPEG")
FFMPEG_BUILD_SCRIPT="${FFMPEG_BUILD_SCRIPT:-build_pixel_ffmpeg.sh}"
DOCKER_ENV+=("-e" "FFMPEG_BUILD_SCRIPT=$FFMPEG_BUILD_SCRIPT")

docker run --rm \
  "${DOCKER_ENV[@]}" \
  -v "$ROOT:/io" \
  "$IMAGE" \
  bash -lc "
    set -euo pipefail
    cd /io
    rm -rf dist wheelhouse codec_video_prep.egg-info compressed_video_preinfer.egg-info
    if [[ \"\${REUSE_FFMPEG:-0}\" != \"1\" ]]; then
      rm -rf build build_ffmpeg_install src/codec_video_prep/libs
    fi
    /opt/python/$PY_TAG/bin/python -m pip install -U pip setuptools wheel build \
      \"numpy<2; python_version < '3.14'\" \
      \"numpy>=2,<3; python_version >= '3.14'\"
    if [[ \"\${REUSE_FFMPEG:-0}\" == \"1\" && -d build_ffmpeg_install/lib && -d src/codec_video_prep/libs ]]; then
      echo 'Reusing existing patched FFmpeg build.'
    else
      bash "$FFMPEG_BUILD_SCRIPT"
    fi
    /opt/python/$PY_TAG/bin/python -m build --wheel
    LD_LIBRARY_PATH=/io/build_ffmpeg_install/lib:/io/src/codec_video_prep/libs \
      auditwheel repair -w wheelhouse dist/*.whl
    auditwheel show wheelhouse/*.whl
    ls -lh wheelhouse
  "
