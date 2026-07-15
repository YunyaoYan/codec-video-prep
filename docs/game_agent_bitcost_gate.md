# Game Agent bitcost-only gate

This note describes the lightweight codec signal used to decide when a game
agent should call a vision model. It intentionally uses only the public
repository's bitcost reader. It does not use motion vectors, optical flow, or
pixel differencing.

## Goal

Instead of sending every video frame to the vision model:

```text
decode codec metadata -> compute bitcost score -> call vision model only when needed
```

The score is a trigger candidate, not a semantic action decision. A production
agent should still add a cooldown and a periodic heartbeat for small UI/state
changes.

## Build the public repository

```bash
git clone https://github.com/YunyaoYan/codec-video-prep.git
cd codec-video-prep
# Activate any Python 3.10+ environment that contains NumPy and setuptools.
# On Debian/Ubuntu, install nasm with your system package manager if needed.
bash build_pixel_ffmpeg.sh
python setup.py build_ext --inplace
```

The build uses the repository's bitcost-only FFmpeg patch. The resulting
reader returns bitcost maps and frame metadata; no MV export is required.

## Run analysis

The branch includes:

```text
scripts/analyze_bitcost_game_agent.py
```

Example:

```bash
export PYTHONPATH="$PWD/src:$PWD:${PYTHONPATH:-}"
export LD_LIBRARY_PATH="$PWD/src/codec_video_prep/libs:$PWD/build_ffmpeg_install/lib:${LD_LIBRARY_PATH:-}"

python scripts/analyze_bitcost_game_agent.py \
  --video_dir /path/to/game-videos \
  --output ./outputs/game_agent_bitcost.json \
  --sample_every 3 \
  --parallel_segments 4 \
  --threads_per_segment 2
```

`sample_every=3` means one codec sample every three video frames. For an online
agent, start with 2--5 frames depending on latency and CPU budget.

## Output and score meaning

The output contains one record per sampled frame:

```json
{
  "frame_idx": 123,
  "mean": 0.42,
  "p90": 1.31,
  "p95": 1.70,
  "p99": 2.10,
  "max": 3.20,
  "pict_type": "P"
}
```

The values are statistics of `log1p(sub_mb_bit_cost)` across the frame's
sub-blocks. `p90` is the 90th percentile: roughly 90% of blocks are at or below
it and roughly 10% are at or above it. It is not a claim that 90% of the image
changed.

For a first gate, use a per-video threshold on `p90` or `mean`:

```python
should_call_agent = frame["p90"] > threshold
```

Choose `threshold` from the video's distribution, then validate it against
actual agent reaction quality. Thresholds should not be assumed identical
across games, codecs, or resolutions.

## Agent-side policy

Do not call the vision model for every frame above threshold. Start with:

```text
high threshold: tune from p90/p95 distribution
cooldown:       300--500 ms
heartbeat:      2--5 s even when score is low
```

Use a high threshold to start an observation opportunity. The agent may keep a
short frame buffer and choose the latest or strongest candidate within the
cooldown window. The heartbeat is needed for health, inventory, text, timer,
and icon changes that may have low codec complexity.

## Limitations

- Bitcost measures codec coding cost/complexity, not game semantics.
- I/P/B frame types have different coding behavior; compare distributions per
  video and be cautious about using a single cross-video threshold.
- This implementation intentionally has no MV, pixel delta, event merging, or
  frame deduplication. Those policies belong in a later Agent runtime layer.
- `cv_reader_fast` must preserve display-order frame indices when selected-frame
  decoding is used.
