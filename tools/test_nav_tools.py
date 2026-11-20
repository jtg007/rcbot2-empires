"""Smoke test for the offline navigation tools: displacement heights must match the navmesh.

Usage: python3 -I tools/test_nav_tools.py [maps_dir]    (default: $EMPIRES_TEST_SERVER/empires/maps)
Skips (exit 0) when emp_palmbay.bsp/.nav are not there.  Guards the displacement grid
orientation in navlib/terrain.py: with the wrong orientation the median difference is ~37 HU.
"""

from __future__ import annotations

import os
from pathlib import Path
import random
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

from navlib.navmesh import read_nav  # noqa: E402
from navlib.terrain import read_displacement_triangles  # noqa: E402


def height_at(tri, x, y):
    (ax, ay, az), (bx, by, bz), (cx, cy, cz) = tri
    d = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy)
    if d == 0:
        return None
    l1 = ((by - cy) * (x - cx) + (cx - bx) * (y - cy)) / d
    l2 = ((cy - ay) * (x - cx) + (ax - cx) * (y - cy)) / d
    l3 = 1 - l1 - l2
    return None if min(l1, l2, l3) < -1e-6 else l1 * az + l2 * bz + l3 * cz


def main() -> int:
    maps = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(os.environ.get("EMPIRES_TEST_SERVER", "")) / "empires" / "maps"
    bsp, nav = maps / "emp_palmbay.bsp", maps / "emp_palmbay.nav"
    if not bsp.exists() or not nav.exists():
        print(f"skip: {bsp} / {nav} not found")
        return 0
    mesh = read_nav(nav)
    triangles = read_displacement_triangles(bsp)
    buckets: dict[tuple[int, int], list] = {}
    for tri in triangles:
        xs, ys = [p[0] for p in tri], [p[1] for p in tri]
        for gx in range(int(min(xs)) // 256, int(max(xs)) // 256 + 1):
            for gy in range(int(min(ys)) // 256, int(max(ys)) // 256 + 1):
                buckets.setdefault((gx, gy), []).append(tri)
    random.seed(1)
    diffs = []
    for area in random.sample(mesh.areas, 300):
        x, y = (area.nw[0] + area.se[0]) / 2, (area.nw[1] + area.se[1]) / 2
        z = (area.nw[2] + area.se[2] + area.ne_z + area.sw_z) / 4
        zs = [h for t in buckets.get((int(x) // 256, int(y) // 256), []) if (h := height_at(t, x, y)) is not None]
        if zs:
            diffs.append(min(abs(h - z) for h in zs))
    diffs.sort()
    median = diffs[len(diffs) // 2]
    print(f"displacement triangles {len(triangles)}, sampled areas {len(diffs)}, median height difference {median:.1f} HU")
    if len(diffs) < 200 or median > 5.0:
        print("FAIL: displacement heights do not match the navmesh")
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
