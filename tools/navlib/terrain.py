"""Displacement terrain triangles from a compiled Source BSP (standard library only).

Generated maps such as emp_palmbay are displacement terrain; walls and cliffs are
steep displacement triangles, not brushes.  This module rebuilds the displacement
surfaces (face corners + dispinfo + displacement vertices) as 3D triangles so the
overview can show height and slope next to the navmesh.
"""

from __future__ import annotations

from pathlib import Path
import struct

from .bsp import BSPReadError, read_bsp_lumps

LUMP_VERTEXES, LUMP_FACES, LUMP_EDGES, LUMP_SURFEDGES, LUMP_DISPINFO, LUMP_DISP_VERTS = 3, 7, 12, 13, 26, 33

Vec3 = tuple[float, float, float]
Triangle = tuple[Vec3, Vec3, Vec3]


def _lerp(a: Vec3, b: Vec3, t: float) -> Vec3:
    return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t)


def read_displacement_triangles(path: str | Path, bounds: tuple[float, float, float, float] | None = None) -> list[Triangle]:
    """All displacement triangles, optionally only those of displacements touching ``bounds`` (x0, y0, x1, y1)."""

    _v, lumps = read_bsp_lumps(
        path, (LUMP_VERTEXES, LUMP_FACES, LUMP_EDGES, LUMP_SURFEDGES, LUMP_DISPINFO, LUMP_DISP_VERTS)
    )
    verts = lumps[LUMP_VERTEXES]
    edges = lumps[LUMP_EDGES]
    surfedges = lumps[LUMP_SURFEDGES]
    faces = lumps[LUMP_FACES]
    disp_verts = lumps[LUMP_DISP_VERTS]
    triangles: list[Triangle] = []
    for index in range(len(lumps[LUMP_DISPINFO]) // 176):
        info = struct.unpack_from("<3fiiiifiH", lumps[LUMP_DISPINFO], index * 176)
        start, vert_start, power, map_face = info[:3], info[3], info[5], info[9]
        if (map_face + 1) * 56 > len(faces):
            raise BSPReadError(f"displacement {index} names face {map_face} outside the faces lump")
        first_edge, num_edges = struct.unpack_from("<ih", faces, map_face * 56 + 4)
        if num_edges != 4:
            continue
        corners: list[Vec3] = []
        for k in range(4):
            (surf,) = struct.unpack_from("<i", surfedges, (first_edge + k) * 4)
            pair = struct.unpack_from("<HH", edges, abs(surf) * 4)
            vertex = pair[0] if surf >= 0 else pair[1]
            corners.append(struct.unpack_from("<3f", verts, vertex * 12))
        nearest = min(range(4), key=lambda i: sum((corners[i][a] - start[a]) ** 2 for a in range(3)))
        c = corners[nearest:] + corners[:nearest]
        if bounds is not None:
            xs = [p[0] for p in c]
            ys = [p[1] for p in c]
            if max(xs) < bounds[0] - 256 or min(xs) > bounds[2] + 256 or max(ys) < bounds[1] - 256 or min(ys) > bounds[3] + 256:
                continue
        n = (1 << power) + 1
        # Displacement vertices are stored column-major relative to the corner order
        # (checked against emp_palmbay.nav: median height difference 1 HU).
        grid: list[Vec3] = []
        for row in range(n):
            left = _lerp(c[0], c[3], row / (n - 1))
            right = _lerp(c[1], c[2], row / (n - 1))
            for col in range(n):
                base = _lerp(left, right, col / (n - 1))
                vx, vy, vz, dist, _alpha = struct.unpack_from("<5f", disp_verts, (vert_start + col * n + row) * 20)
                grid.append((base[0] + vx * dist, base[1] + vy * dist, base[2] + vz * dist))
        for row in range(n - 1):
            for col in range(n - 1):
                a, b = grid[row * n + col], grid[row * n + col + 1]
                d, e = grid[(row + 1) * n + col], grid[(row + 1) * n + col + 1]
                triangles.append((a, b, d))
                triangles.append((b, e, d))
    return triangles


def slope_degrees(tri: Triangle) -> float:
    """Angle between the triangle plane and the horizontal, 0 = flat, 90 = wall."""

    import math

    (ax, ay, az), (bx, by, bz), (cx, cy, cz) = tri
    ux, uy, uz = bx - ax, by - ay, bz - az
    vx, vy, vz = cx - ax, cy - ay, cz - az
    nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
    length = math.sqrt(nx * nx + ny * ny + nz * nz)
    if length == 0.0:
        return 90.0
    return math.degrees(math.acos(min(1.0, abs(nz) / length)))
