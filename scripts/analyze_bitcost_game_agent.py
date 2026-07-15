#!/usr/bin/env python3
"""Extract lightweight per-frame bitcost scores for a Game Agent gate."""
import argparse
import json
from pathlib import Path

import numpy as np

from codec_selector.codec_patch_gop.video_probe import get_total_frames_fps
from codec_selector.codec_patch_gop.video_processor import cv_reader_fetch_bitcost


VIDEO_EXTS = {".mp4", ".mov", ".mkv", ".avi", ".webm"}


def score_item(item):
    values = item.get("sub_mb_bit_cost")
    if values is None:
        return None
    values = np.log1p(np.maximum(np.asarray(values, dtype=np.float32), 0.0))
    return {
        "frame_idx": int(item["frame_idx"]),
        "mean": float(values.mean()),
        "p90": float(np.percentile(values, 90)),
        "p95": float(np.percentile(values, 95)),
        "p99": float(np.percentile(values, 99)),
        "max": float(values.max()),
        "pict_type": item.get("pict_type", ""),
    }


def analyze_video(path, sample_every, parallel_segments, threads_per_segment):
    total_frames, fps, height, width = get_total_frames_fps(str(path))
    frame_ids = list(range(0, total_frames, max(1, int(sample_every))))
    items = cv_reader_fetch_bitcost(
        str(path), frame_ids, bitcost_grid="sub",
        parallel_segments=int(parallel_segments),
        threads_per_segment=int(threads_per_segment),
    )
    frames = [s for s in (score_item(item) for item in items) if s is not None]
    return {
        "video_path": str(path),
        "total_frames": int(total_frames),
        "fps": float(fps),
        "height": int(height),
        "width": int(width),
        "sample_every": int(sample_every),
        "analyzed_frames": len(frames),
        "frames": frames,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--video_dir", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--sample_every", type=int, default=3)
    parser.add_argument("--parallel_segments", type=int, default=4)
    parser.add_argument("--threads_per_segment", type=int, default=2)
    args = parser.parse_args()

    root = Path(args.video_dir)
    paths = [root] if root.is_file() else sorted(
        p for p in root.rglob("*") if p.is_file() and p.suffix.lower() in VIDEO_EXTS
    )
    results = []
    for index, path in enumerate(paths, 1):
        print(f"[{index}/{len(paths)}] {path}", flush=True)
        try:
            results.append(analyze_video(
                path, args.sample_every, args.parallel_segments,
                args.threads_per_segment,
            ))
        except Exception as exc:
            results.append({"video_path": str(path), "error": repr(exc)})

    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", encoding="utf-8") as handle:
        json.dump({"mode": "bitcost_only", "videos": results}, handle,
                  indent=2, ensure_ascii=False)
    print(f"[done] wrote {output}")


if __name__ == "__main__":
    main()
