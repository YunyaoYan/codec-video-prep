#!/usr/bin/env python3
"""Regression check for selected-frame bitcost across B-frame reordering."""

from __future__ import annotations

import subprocess
import tempfile
from pathlib import Path

from codec_video_prep import cv_reader_fast


def main() -> None:
    with tempfile.TemporaryDirectory(prefix='codec-selected-bitcost-') as tmp:
        video = Path(tmp) / 'bframes.mp4'
        subprocess.run(
            [
                'ffmpeg',
                '-hide_banner',
                '-loglevel',
                'error',
                '-y',
                '-f',
                'lavfi',
                '-i',
                'testsrc2=size=96x64:rate=24',
                '-frames:v',
                '48',
                '-c:v',
                'libx264',
                '-bf',
                '3',
                '-g',
                '24',
                '-pix_fmt',
                'yuv420p',
                str(video),
            ],
            check=True,
            timeout=60,
        )
        frame_ids = [0, 5, 11, 17, 23, 29, 35, 41, 47]
        frames = cv_reader_fast.read_video_fast_selected(
            path=str(video),
            frame_ids=frame_ids,
            thread_count=1,
            export_bitcost=1,
            export_pixels=1,
            out_w=96,
            out_h=64,
            thread_type='slice',
        )
        returned_ids = [int(frame['frame_idx']) for frame in frames]
        if returned_ids != frame_ids:
            raise RuntimeError(f'selected decoder returned IDs {returned_ids}, expected {frame_ids}')
        missing = [int(frame['frame_idx']) for frame in frames if not frame.get('bitcost')]
        if missing:
            raise RuntimeError(f'selected decoder omitted bitcost for frames {missing}')
        if not any(frame.get('pict_type') == 'B' for frame in frames):
            raise RuntimeError('regression fixture did not sample any B-frame')
        print(f'selected bitcost check passed: {len(frames)} frames')


if __name__ == '__main__':
    main()
