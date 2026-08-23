"""Generate a dependency-free YUV4MPEG test clip for VLC/PHY smoke tests."""

from __future__ import annotations

import argparse
import random
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument("--seconds", type=int, default=10)
    parser.add_argument("--fps", type=int, default=15)
    parser.add_argument("--width", type=int, default=320)
    parser.add_argument("--height", type=int, default=180)
    parser.add_argument(
        "--noise",
        action="store_true",
        help="generate deterministic high-entropy frames for bitrate stress tests",
    )
    args = parser.parse_args()
    if args.seconds <= 0 or args.fps <= 0:
        raise ValueError("seconds and fps must be positive")
    if args.width <= 0 or args.height <= 0 or args.width % 2 or args.height % 2:
        raise ValueError("width and height must be positive even numbers")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    chroma_width = args.width // 2
    chroma_height = args.height // 2
    frames = args.seconds * args.fps
    noise = random.Random(0x15AC)
    raw_output = args.output.suffix.lower() in {".yuv", ".i420"}
    with args.output.open("wb") as stream:
        if not raw_output:
            stream.write(
                f"YUV4MPEG2 W{args.width} H{args.height} F{args.fps}:1 Ip A1:1 C420jpeg\n".encode()
            )
        for frame in range(frames):
            if not raw_output:
                stream.write(b"FRAME\n")
            if args.noise:
                stream.write(noise.randbytes(args.width * args.height))
                stream.write(noise.randbytes(chroma_width * chroma_height))
                stream.write(noise.randbytes(chroma_width * chroma_height))
            else:
                for y in range(args.height):
                    row = bytearray(args.width)
                    for x in range(args.width):
                        checker = (
                            55 if ((x // 16 + y // 16 + frame // 2) & 1) else -55
                        )
                        bar = 70 if abs(x - ((frame * 7) % args.width)) < 10 else 0
                        row[x] = max(16, min(235, 126 + checker + bar))
                    stream.write(row)
                u_value = 64 + (frame * 3) % 128
                v_value = 192 - (frame * 5) % 128
                stream.write(bytes([u_value]) * (chroma_width * chroma_height))
                stream.write(bytes([v_value]) * (chroma_width * chroma_height))
    print(
        f"Wrote {args.output} ({'noise ' if args.noise else ''}"
        f"{'raw I420' if raw_output else 'YUV4MPEG2'}, "
        f"{args.width}x{args.height}, {args.fps} fps, "
        f"{args.seconds} s, {frames} frames)"
    )


if __name__ == "__main__":
    main()
