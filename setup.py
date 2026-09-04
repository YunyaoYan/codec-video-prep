"""setuptools configuration for codec-video-prep.

This file handles the C++ extension build and FFmpeg library bundling.
PEP 517 metadata is defined in pyproject.toml.
"""

from pathlib import Path

import numpy
from setuptools import Extension, find_packages, setup

ROOT = Path(__file__).parent
ffmpeg_root = ROOT / "build_ffmpeg_install"

extra_link_args = [
    f"-L{ffmpeg_root / 'lib'}",
    "-Wl,--no-as-needed",
    "-lavformat",
    "-lavcodec",
    "-lswscale",
    "-lavutil",
    "-Wl,--as-needed",
    "-Wl,-rpath,$ORIGIN/libs",
    "-Wl,-Bsymbolic",
]

ext = Extension(
    "codec_video_prep.cv_reader_fast",
    sources=["native/cv_reader_fast.cpp"],
    include_dirs=[
        str(ffmpeg_root / "include"),
        numpy.get_include(),
    ],
    library_dirs=[],
    libraries=[],
    language="c++",
    extra_compile_args=["-std=c++11", "-O3"],
    extra_link_args=extra_link_args,
)

# Packages from src/ (codec_video_prep, compressed_video_preinfer)
src_packages = find_packages("src")

# codec_selector is still part of the runtime pipeline used by codec_video_prep.api.
root_packages = find_packages(".", include=["codec_selector", "codec_selector.*"])

packages = src_packages + root_packages

package_dir = {
    "codec_video_prep": "src/codec_video_prep",
    "compressed_video_preinfer": "src/compressed_video_preinfer",
}

setup(
    name="codec-video-prep",
    version="0.2.6",
    description="Codec-aware video preprocessing for training and inference",
    python_requires=">=3.10",
    install_requires=[
        "numpy>=1.23,<2.0; python_version < '3.14'",
        "numpy>=2.0,<3.0; python_version >= '3.14'",
        "opencv-python-headless<4.12",
        "Pillow",
    ],
    entry_points={
        "console_scripts": [
            "codec-video-prep=codec_video_prep.cli:main",
            "codec-video-prep-doctor=codec_video_prep.doctor:main",
            "cv-preinfer=compressed_video_preinfer.cli:main",
            "cv-preinfer-doctor=compressed_video_preinfer.doctor:main",
        ],
    },
    packages=packages,
    package_dir=package_dir,
    package_data={
        "codec_video_prep": ["libs/*.so*"],
        "codec_selector": [],
    },
    ext_modules=[ext],
    zip_safe=False,
)
