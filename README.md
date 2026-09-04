# codec-video-prep (v0.2.6)

Codec-aware video preprocessing for training and inference. Extracts codec-level bitcost information from **H.264 / HEVC / VP9** videos and turns them into patch-canvases ready for downstream vision models.

## What it does

- **Pixel-capable patched FFmpeg decoder** – Instruments the H.264 / HEVC / VP9 decoder to export per-macroblock (H.264) or per-CTU (HEVC) **bitcost** maps while keeping normal reconstruction available.
- **Fast C++ extension** (`cv_reader_fast`) – Runs selected-frame decoding and codec-feature collection in the same scan, returning both BGR pixels and bitcost data as NumPy arrays.
- **Readiness grouping** – Groups frames by compressibility (bitcost) so that hard-to-decode regions get more patches.
- **Top-K patch selection** – Selects the most informative 2×2 patch blocks from each group and packs them into JPG/PNG canvases.
- **One-command pipeline** – From a raw video to a folder of canvases + metadata in a single call.

---

## Install

### From PyPI (recommended)

```bash
python -m pip install codec-video-prep
```

Verify the installation:

```bash
codec-video-prep-doctor
```

### From wheel file

```bash
python -m pip install codec_video_prep-0.2.6-*.whl
```

### Build from source

1. Build the pixel-capable patched FFmpeg shared libraries:

   ```bash
   bash build_pixel_ffmpeg.sh
   ```

   This is the primary build path. It preserves reconstructed pixels and exposes codec bitcost data, which lets `cv_reader_fast` fetch pixels and codec features in one decode scan.

2. Build and install the Python package:

```bash
python -m pip install -e .
```

---

## CLI Usage (`codec-video-prep`)

### Quick start

```bash
codec-video-prep \
  --video /path/to/video.mp4 \
  --out_dir ./preinfer_out \
  --num_sampled_frames 1024 \
  --group_size 32 \
  --images_per_group 4 \
  --patch 14 \
  --max_pixels 153664
```

### Full parameter list

#### Input / Output

| Parameter | Default | Description |
|---|---|---|
| `--video` | **required** | Path to input video file |
| `--out_dir` | **required** | Output directory for canvases and metadata |
| `--canvas_format` | `jpg` | Canvas image format: `jpg`, `png`, or `npy` |
| `--save_mask_video` | `False` | Save a side-by-side mask visualization video |

#### Frame Sampling

| Parameter | Default | Description |
|---|---|---|
| `--frame_sampling_mode` | `uniform_count` | How to sample frames: `fps`, `uniform_count`, `pkt_peak` |
| `--sample_fps` | `4.0` | Target FPS when `frame_sampling_mode=fps` |
| `--num_sampled_frames` | `1024` | Exact number of frames to uniformly sample when `frame_sampling_mode=uniform_count` |
| `--avoid_keyframes` / `--no_avoid_keyframes` | `True` | Shift sampled frames away from keyframes to avoid decoder drift |

#### Grouping

| Parameter | Default | Description |
|---|---|---|
| `--grouping_mode` | `readiness` | Grouping strategy: `readiness` (dynamic) or `fixed` (fixed-size) |
| `--group_size` | `32` | Max frames per group (for `fixed` mode or readiness window) |
| `--images_per_group` | `4` | Number of patch canvases to extract per group |
| `--min_group_frames` | `8` | Minimum frames per readiness group |
| `--max_group_frames` | `64` | Maximum frames per readiness group |

#### Readiness Threshold (when `--grouping_mode readiness`)

| Parameter | Default | Description |
|---|---|---|
| `--readiness_sum_threshold_mode` | `legacy` | Threshold mode: `legacy`, `auto`, `fixed`, `clamped_sqrt_bpppf` |
| `--readiness_sum_threshold` | `0.0` | Fixed threshold (used by `legacy` and `fixed` modes) |
| `--readiness_norm_sum_threshold` | `2250000.0` | Normalized threshold (used by `clamped_sqrt_bpppf` mode) |
| `--readiness_coverage_bins` | `3` | Minimum temporal bins that selected patches must cover |
| `--readiness_delta_ratio` | `0.05` | Stop extending group when score gain drops below this ratio |

#### Resolution

| Parameter | Default | Description |
|---|---|---|
| `--patch` | `14` | Vision model patch size (e.g. 14 for ViT) |
| `--max_pixels` | `153664` | Max pixels per canvas (resize limit) |
| `--max_dim` | `616` | Max dimension (width or height) before resize |
| `--block_size` | `2` | Block size for patch grouping (2×2 or 3×3) |
| `--no_resize` | `False` | Disable resize entirely |

#### Bitcost Scoring

| Parameter | Default | Description |
|---|---|---|
| `--bitcost_grid` | `adaptive` | Bitcost granularity: `sub`, `mb`, `ctu`, `adaptive` |
| `--bitcost_pct` | `99.0` | Percentile for bitcost normalization |
| `--bitcost_log_scale` / `--no_bitcost_log_scale` | `True` | Apply log scale to bitcost scores |
| `--disable_target_only` | `False` | Disable decoder-internal target-frame-only bitcost pruning |

#### Decode Backend

| Parameter | Default | Description |
|---|---|---|
| `--decode_backend` | `cv_reader_pixels` | Decoder backend: `ffmpeg_native` or `cv_reader_pixels` |
| `--parallel_segments` | `0` | Number of parallel decode segments (0 = serial) |
| `--threads_per_segment` | `4` | FFmpeg thread count per segment worker |
| `--segment_guard_frames` | `30` | Extra frames around segment boundaries for keyframe-seek safety |
| `--thread_type` | `slice` | FFmpeg decoder thread type: `auto`, `slice`, or `frame` |
| `--thread_count` | `1` | FFmpeg decoder thread count for `cv_reader_fast` |

### CLI Example: Reproduce legacy benchmark settings

```bash
for id in 001 002 003 004 005 006 007 008 009 010; do
  codec-video-prep \
    --video /path/to/${id}.mp4 \
    --out_dir ./output/${id} \
    --num_sampled_frames 512 \
    --group_size 32 \
    --images_per_group 4 \
    --patch 14 \
    --max_pixels 313600 \
    --min_group_frames 8 \
    --max_group_frames 128 \
    --bitcost_grid sub \
    --grouping_mode readiness \
    --frame_sampling_mode uniform_count \
    --readiness_sum_threshold_mode auto \
    --decode_backend cv_reader_pixels \
    --no_avoid_keyframes \
    --parallel_segments 32 \
    --threads_per_segment 1 \
    --disable_target_only
done
```

### Output files

After running, the output directory contains:

| File | Description |
|---|---|
| `canvas_*.jpg` | Packed patch canvases |
| `meta.json` | Full metadata, config, timing breakdown, and group info |
| `frame_ids.npy` | Sampled frame indices |
| `src_patch_position.npy` | Source patch positions `(group, patch, y1, x1, y2, x2)` |

### Decode backends

Two decode backends are available:

| Backend | Description | Best for |
|---------|-------------|----------|
| `cv_reader_pixels` (default) | Single-pass decode via `cv_reader_fast` that returns **both** bitcost and BGR pixels | Recommended path |
| `ffmpeg_native` | FFmpeg subprocess pixel decode + `cv_reader_fast` bitcost scan | Compatibility/debugging |

Use the default backend explicitly:

```bash
codec-video-prep --decode_backend cv_reader_pixels ...
```

### Parallel segment decoding

For long videos with dense frame sampling, the `cv_reader_fast` scan can be split into N parallel decode segments using `ProcessPoolExecutor`:

```bash
codec-video-prep \
  --decode_backend cv_reader_pixels \
  --parallel_segments 4 \
  --threads_per_segment 4 \
  --segment_guard_frames 30 \
  ...
```

| Parameter | Default | Description |
|-----------|---------|-------------|
| `--parallel_segments` | `0` (disabled) | Number of parallel segments. Set to `0` or `1` to use serial decoding. |
| `--threads_per_segment` | `4` | FFmpeg `thread_count` inside each worker process. |
| `--segment_guard_frames` | `30` | Extra frames decoded before/after each segment boundary to compensate for seek-to-keyframe inaccuracy. |

> **Note:** Parallel segment decoding incurs process-spawn overhead. For short clips (< a few thousand frames) serial decoding is usually faster. The benefit appears on long videos with dense sampling (e.g. 10k+ frames).

---

## Python API

### High-level one-shot call (`run_preinfer`)

```python
from codec_video_prep import run_preinfer

result = run_preinfer(
    video="/path/to/video.mp4",
    out_dir="./preinfer_out",
    num_sampled_frames=1024,
    group_size=32,
    images_per_group=4,
    patch=14,
    max_pixels=153664,
    min_group_frames=8,
    max_group_frames=64,
    bitcost_grid="adaptive",
    decode_backend="cv_reader_pixels",   # or "ffmpeg_native"
    parallel_segments=4,                  # 0 = serial
    threads_per_segment=4,
    segment_guard_frames=30,
)

print(result.out_dir)       # output directory path
print(result.meta_path)     # path to meta.json
print(result.canvas_files)  # list of canvas image paths
print(result.timings)       # dict of timing breakdowns
```

**All parameters mirror the CLI arguments.**

### Using `PreinferConfig` directly

```python
from codec_video_prep import run_preinfer_config, PreinferConfig

cfg = PreinferConfig(
    video="/path/to/video.mp4",
    out_dir="./preinfer_out",
    num_sampled_frames=512,
    group_size=32,
    images_per_group=4,
    patch=14,
    max_pixels=313600,
    decode_backend="cv_reader_pixels",
    parallel_segments=32,
    threads_per_segment=1,
)

result = run_preinfer_config(cfg)
```

### Low-level fast decoder (`cv_reader_fast`)

```python
from codec_video_prep import cv_reader_fast

# Decode ALL frames with bitcost export
frames = cv_reader_fast.read_video_fast(
    path="/path/to/video.mp4",
    thread_count=16,
    export_bitcost=1,
    thread_type="auto",   # "auto" selects "slice" when export_bitcost=1
)

# Decode SELECTED frames only (bitcost + optional pixels)
selected = cv_reader_fast.read_video_fast_selected(
    path="/path/to/video.mp4",
    frame_ids=[0, 30, 60, 90],
    thread_count=16,
    export_bitcost=1,
    export_pixels=1,      # also return BGR pixels
    out_w=224,            # optional resize width
    out_h=224,            # optional resize height
    thread_type="slice",  # recommended for bitcost stability
)

# Segment seek + decode (used internally for parallel workers)
segment = cv_reader_fast.read_video_fast_selected_segment(
    path="/path/to/video.mp4",
    frame_ids=[30, 60, 90],
    seek_frame=0,         # seek target (decoder lands on nearest keyframe before this)
    end_frame=120,        # stop after this frame index
    thread_count=4,
    export_bitcost=1,
    export_pixels=1,
    out_w=224,
    out_h=224,
)
```

Each returned frame dict contains:

| Key | Type | Description |
|---|---|---|
| `frame_idx` | `int` | Frame index |
| `pict_type` | `str` | `'I'`, `'P'` or `'B'` |
| `width` / `height` | `int` | Frame resolution |
| `codec_name` | `str` | Decoder name (`h264`, `hevc`, `vp9`, ...) |
| `bitcost` | `dict` | MB/CTU bitcost arrays (when `export_bitcost=1`) |
| `pixels` | `np.ndarray` | `(H, W, 3)` uint8 BGR array (when `export_pixels=1`) |

The `bitcost` dict has one or more of these keys depending on codec and grid:

| Key | Shape | Description |
|---|---|---|
| `mb_bit_cost` | `(mb_h, mb_w)` | Macroblock-level bitcost (H.264) |
| `ctu_bit_cost` | `(ctu_h, ctu_w)` | CTU-level bitcost (HEVC/VP9) |
| `sub_mb_bit_cost` | `(sub_h, sub_w)` | Sub-block bitcost (finer granularity) |

### Important: Threading mode for bitcost

When `export_bitcost=1`, always use `thread_type="slice"` (or `"auto"` which automatically selects `"slice"` for HEVC/H.264). **Frame threading (`"frame"`) can drop `opaque_ref` under the `bitcost_only` patch**, causing some frames to return empty bitcost.

```python
# Correct — stable bitcost
selected = cv_reader_fast.read_video_fast_selected(
    path="video.mp4",
    frame_ids=[0, 10, 20],
    export_bitcost=1,
    thread_type="slice",
)

# Risky — may lose bitcost on some frames
selected = cv_reader_fast.read_video_fast_selected(
    path="video.mp4",
    frame_ids=[0, 10, 20],
    export_bitcost=1,
    thread_type="frame",
)
```

---

## Project structure

```
├── src/codec_video_prep/    # Main Python package
│   ├── __init__.py          # Package exports
│   ├── api.py               # run_preinfer() entrypoint
│   ├── cli.py               # codec-video-prep CLI
│   ├── config.py            # PreinferConfig
│   ├── doctor.py            # codec-video-prep-doctor diagnostics
│   └── libs/                # Bundled FFmpeg shared libraries
├── src/compressed_video_preinfer/  # Backward-compatible alias
├── codec_selector/          # Frame sampling / grouping / patch selection
│   ├── core/                # Pipeline, probe, decode, config
│   ├── plugins/             # Extensible samplers, scorers, groupers
│   └── codec_patch_gop/     # Legacy GOP-based utilities
├── native/                  # C++ Python extension
│   └── cv_reader_fast.cpp   # Fast decoder with bitcost + pixel export
├── ffmpeg_patch/            # FFmpeg source patches
│   └── bitcost_only/        # Main pixel-capable patches (H.264 + HEVC + VP9)
├── scripts/                 # Build scripts
│   └── build_all_manylinux_*.sh
├── build_pixel_ffmpeg.sh    # Builds the patched FFmpeg used by wheels/source installs
├── setup.py                 # setuptools build (C++ extension + FFmpeg libs)
├── pyproject.toml           # PEP 517 project metadata
└── README.md                # This file
```

---

## Build a manylinux wheel

The manylinux scripts use `build_pixel_ffmpeg.sh` by default, so wheels are built with the one-scan pixel + bitcost decoder path.

```bash
# Build cp310 first (compiles FFmpeg)
PY_TAG=cp310-cp310 bash scripts/build_manylinux_wheel.sh

# Build remaining versions reusing FFmpeg
REUSE_FFMPEG=1 PY_TAG=cp311-cp311 bash scripts/build_manylinux_wheel.sh
REUSE_FFMPEG=1 PY_TAG=cp312-cp312 bash scripts/build_manylinux_wheel.sh
REUSE_FFMPEG=1 PY_TAG=cp313-cp313 bash scripts/build_manylinux_wheel.sh
```

Output:

```
wheelhouse/codec_video_prep-0.2.6-cp311-cp311-manylinux2014_x86_64.manylinux_2_17_x86_64.whl
```

## Diagnostics

```bash
codec-video-prep-doctor
```

Checks:
- `cv_reader_fast` C extension can be imported
- Bundled FFmpeg shared libraries are present
- Threading defaults (`slice` for bitcost, 1 thread)

---

## Backward Compatibility

The old import path and CLI names are kept as aliases:

- `compressed_video_preinfer`
- `cv-preinfer`
- `cv-preinfer-doctor`

---

## Requirements

- Python ≥ 3.10
- numpy >= 1.23, < 2.0
- opencv-python-headless < 4.12
- Pillow
- Patched FFmpeg shared libraries (bundled in the wheel or built from `build_pixel_ffmpeg.sh`)
