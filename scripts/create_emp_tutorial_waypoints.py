#!/usr/bin/env python3
"""Create the minimal navigation graph used by the isolated tutorial proof."""

from pathlib import Path
import struct
import sys


def cstr(value: str, size: int) -> bytes:
    raw = value.encode("ascii")[: size - 1]
    return raw + b"\0" * (size - len(raw))


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} OUTPUT.rcw")

    # These are the walkable tutorial corridor/room anchors, in order from the
    # BE tutorial spawn to its exit trigger. Links are deliberately one-way so
    # the proof follows the tutorial route deterministically.
    points = [
        (-10008.0, -7886.0, 4312.0),
        # The tutorial has a thin invisible brush across the direct x-route
        # around x=-9610. Walk above it, then come back to the welcome trigger.
        (-10008.0, -7600.0, 4312.0),
        (-9306.0, -7600.0, 4312.0),
        (-9306.0, -7896.0, 4312.0),
        (-8400.0, -8544.0, 4312.0),
        (-8516.0, -9168.0, 4312.0),
        (-8516.0, -9400.0, 4312.0),
        (-8516.0, -9728.0, 4312.0),
        (-8068.0, -9188.0, 4312.0),
        (-7960.0, -8764.0, 4312.0),
        (-8068.0, -6793.0, 4312.0),
        (-8160.0, -6016.0, 4312.0),
        (-8160.0, -5472.0, 4312.0),
        (-8164.0, -573.0, 4356.0),
    ]

    output = Path(sys.argv[1])
    data = bytearray()
    data += struct.pack(
        "<16s64siii",
        cstr("RCBot2", 16),
        cstr("emp_tutorial_2015_fix", 64),
        5,
        len(points),
        0,
    )
    data += cstr("isolated-tutorial-proof", 32)
    data += cstr("codex", 32)

    for index, (x, y, z) in enumerate(points):
        paths = [index + 1] if index + 1 < len(points) else []
        data += struct.pack("<3fii?i", x, y, z, 0, 0, True, len(paths))
        for target in paths:
            data += struct.pack("<i", target)
        data += struct.pack("<if", 0, 0.0)

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(data)
    print(f"wrote {output} ({len(data)} bytes, {len(points)} waypoints)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
