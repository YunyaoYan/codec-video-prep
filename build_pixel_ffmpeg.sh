#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD="$ROOT/build/ffmpeg_pixel"
INSTALL="$ROOT/build_ffmpeg_install"
PATCHDIR="$ROOT/ffmpeg_patch/bitcost_only"

rm -rf "$BUILD" "$INSTALL"
mkdir -p "$BUILD"

tar xf "$ROOT/ffmpeg/ffmpeg-snapshot.tar.bz2" -C "$BUILD" --strip-components=1

cd "$BUILD"

# Copy pure-additive bitcost collectors (no reconstruction skipping)
cp "$PATCHDIR"/h264_cavlc.c libavcodec/
cp "$PATCHDIR"/h264_cabac.c libavcodec/
cp "$PATCHDIR"/hevcdec.c libavcodec/
cp "$PATCHDIR"/hevcdec.h libavcodec/
cp "$PATCHDIR"/hevc_refs.c libavcodec/
cp "$PATCHDIR"/vp9.c libavcodec/
cp "$PATCHDIR"/vp9shared.h libavcodec/
cp "$PATCHDIR"/vp9dec.h libavcodec/

# Apply header/struct patch for bitcost_buf (H.264 only; HEVC structs are in hevcdec.h)
patch -p1 < "$PATCHDIR/h264_bitcost_only.patch"

# The bundled FFmpeg snapshot predates GCC's stricter validation of x86 shift
# constraints. Python 3.14 wheels use a newer manylinux toolchain.
patch -p1 < "$PATCHDIR/modern_gcc_mathops.patch"

./configure   --prefix="$INSTALL"   --enable-shared   --disable-static   --disable-programs   --disable-doc   --disable-debug   --enable-avcodec   --enable-avformat   --enable-avutil   --enable-swresample   --enable-swscale   --enable-protocol=file   --enable-demuxer=mov   --enable-demuxer=matroska   --enable-demuxer=h264   --enable-demuxer=hevc   --enable-parser=h264   --enable-parser=hevc   --enable-decoder=h264   --enable-decoder=hevc   --enable-decoder=vp9

NPROC=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
make -j"$NPROC"
make install

mkdir -p "$ROOT/src/codec_video_prep/libs"
rm -f "$ROOT/src/codec_video_prep/libs/"* 2>/dev/null || true

cp -P "$INSTALL"/lib/libavcodec.so* "$ROOT/src/codec_video_prep/libs/"
cp -P "$INSTALL"/lib/libavformat.so* "$ROOT/src/codec_video_prep/libs/"
cp -P "$INSTALL"/lib/libavutil.so* "$ROOT/src/codec_video_prep/libs/"
cp -P "$INSTALL"/lib/libswresample.so* "$ROOT/src/codec_video_prep/libs/" 2>/dev/null || true
cp -P "$INSTALL"/lib/libswscale.so* "$ROOT/src/codec_video_prep/libs/"

echo "Pixel-capable FFmpeg build complete: $INSTALL"
