"""Offline reader and connectivity checks for Source engine navigation meshes.

Reads ``.nav`` files of format version 16, subversion 0 (Source SDK 2013
``nav_file.cpp`` / ``nav_area.cpp`` / ``nav_ladder.cpp`` layout, written from
the SDK description, not copied from it).  Only the standard library is used.

Every file is parsed to its exact end: trailing bytes, truncated records and
connection ids that do not name an existing area are structured errors
(:class:`NavReadError`), never silent partial results.

Graph semantics:
- ``NavArea.connections`` holds the ground links of the four directions.
- ``NavArea.ladder_connections`` holds the two ladder links.  Graph functions
  ignore ladders unless ``include_ladders=True``.
- :func:`components` treats links as undirected; :func:`reachable_from`
  follows them in their stored direction only.

Nothing here is Empires-specific.  Offline test aid, copied from the terrain
generator project; not used by the bot plugin.
"""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass, field
from math import sqrt
from pathlib import Path
import struct



class SourceExportError(ValueError):
    """Structured failure with a code and a path."""

    def __init__(self, code: str, message: str, path: str = "navmesh") -> None:
        self.code = code
        self.message = message
        self.path = path
        super().__init__(f"{path}: {message} ({code})")

NAV_MAGIC = 0xFEEDFACE
SUPPORTED_NAV_VERSION = 16
SUPPORTED_NAV_SUBVERSION = 0
NUM_DIRECTIONS = 4
NUM_LADDER_DIRECTIONS = 2
NUM_CORNERS = 4

Vec3 = tuple[float, float, float]

_U8 = struct.Struct("<B")
_U16 = struct.Struct("<H")
_U32 = struct.Struct("<I")
_F32 = struct.Struct("<f")
_VEC3 = struct.Struct("<3f")
_HIDING_SPOT = struct.Struct("<I3fB")  # id, position, flags
_ENCOUNTER_HEAD = struct.Struct("<IBIBB")  # from, from dir, to, to dir, spot count
_ENCOUNTER_SPOT = struct.Struct("<IB")  # area id, parametric distance
_VISIBLE_AREA = struct.Struct("<IB")  # area id, attributes
_LADDER = struct.Struct("<If3f3ffI5I")  # id, width, top, bottom, length, dir, 5 area ids
_EARLIEST_TIMES = struct.Struct("<2f")
_LIGHT_INTENSITY = struct.Struct("<4f")


class NavReadError(SourceExportError):
    """Raised when a .nav file cannot be read or fails structural validation."""

    def __init__(self, code: str, message: str, path: str = "navmesh") -> None:
        super().__init__(code, message, path)


@dataclass(frozen=True)
class NavArea:
    id: int
    nw: Vec3  # north-west corner (minimum x and y)
    se: Vec3  # south-east corner (maximum x and y)
    ne_z: float  # height at (se.x, nw.y)
    sw_z: float  # height at (nw.x, se.y)
    connections: tuple[int, ...]  # ground links, all four directions flattened
    attributes: int
    ladder_connections: tuple[int, ...] = ()  # ladder links, up and down flattened


@dataclass(frozen=True)
class NavMesh:
    version: int
    subversion: int
    areas: tuple[NavArea, ...]
    ladder_count: int = 0


@dataclass(frozen=True)
class NearestArea:
    area_id: int
    inside: bool  # True when the point lies in the area's x/y footprint
    distance: float  # from the point to the area surface (0 when on it)
    center_distance: float  # from the point to the area center


@dataclass(frozen=True)
class PointLocation:
    name: str
    position: Vec3
    area_id: int
    component: int  # index into components(mesh); 0 is the largest
    inside: bool
    distance: float
    center_distance: float


@dataclass(frozen=True)
class PairReachability:
    a: str
    b: str
    a_to_b: bool
    b_to_a: bool

    @property
    def both(self) -> bool:
        return self.a_to_b and self.b_to_a


@dataclass(frozen=True)
class PointReport:
    locations: dict[str, PointLocation]
    pairs: tuple[PairReachability, ...]
    component_count: int
    component_sizes: tuple[int, ...] = field(default=())

    @property
    def all_mutually_reachable(self) -> bool:
        return all(pair.both for pair in self.pairs)


class _Cursor:
    __slots__ = ("data", "pos", "path")

    def __init__(self, data: bytes, path: Path) -> None:
        self.data = data
        self.pos = 0
        self.path = str(path)

    def take(self, record: struct.Struct) -> tuple:
        end = self.pos + record.size
        if end > len(self.data):
            raise NavReadError(
                "nav_truncated",
                f"unexpected end of file at byte {self.pos} (needed {record.size} bytes, "
                f"file has {len(self.data)})",
                self.path,
            )
        values = record.unpack_from(self.data, self.pos)
        self.pos = end
        return values

    def count(self) -> int:
        (value,) = self.take(_U32)
        return value

    def skip(self, length: int) -> None:
        end = self.pos + length
        if end > len(self.data):
            raise NavReadError("nav_truncated", f"unexpected end of file at byte {self.pos}", self.path)
        self.pos = end


def read_nav(path: str | Path) -> NavMesh:
    """Parse a Source ``.nav`` file (version 16, subversion 0) completely."""

    file_path = Path(path)
    try:
        data = file_path.read_bytes()
    except OSError as error:
        raise NavReadError("nav_io", f"cannot read nav file: {error}", str(file_path)) from error

    cursor = _Cursor(data, file_path)
    (magic,) = cursor.take(_U32)
    if magic != NAV_MAGIC:
        raise NavReadError("nav_bad_magic", f"magic 0x{magic:08X} is not 0xFEEDFACE", cursor.path)
    (version,) = cursor.take(_U32)
    if version != SUPPORTED_NAV_VERSION:
        raise NavReadError(
            "nav_unsupported_version",
            f"nav version {version} is not supported (only {SUPPORTED_NAV_VERSION})",
            cursor.path,
        )
    (subversion,) = cursor.take(_U32)
    if subversion != SUPPORTED_NAV_SUBVERSION:
        raise NavReadError(
            "nav_unsupported_subversion",
            f"nav subversion {subversion} has custom data that is not implemented",
            cursor.path,
        )
    cursor.skip(4)  # bspSize: informational, not compared against the BSP here
    cursor.skip(1)  # isAnalyzed
    (place_count,) = cursor.take(_U16)
    for _ in range(place_count):
        (length,) = cursor.take(_U16)
        cursor.skip(length)
    cursor.skip(1)  # hasUnnamedAreas
    area_count = cursor.count()

    areas = tuple(_read_area(cursor) for _ in range(area_count))
    ladder_count = cursor.count()
    for _ in range(ladder_count):
        cursor.take(_LADDER)

    if cursor.pos != len(data):
        raise NavReadError(
            "nav_trailing_bytes",
            f"parser stopped at byte {cursor.pos} but file has {len(data)} bytes",
            cursor.path,
        )

    mesh = NavMesh(version=version, subversion=subversion, areas=areas, ladder_count=ladder_count)
    _validate(mesh, cursor.path)
    return mesh


def _read_area(cursor: _Cursor) -> NavArea:
    (area_id,) = cursor.take(_U32)
    (attributes,) = cursor.take(_U32)
    nw = cursor.take(_VEC3)
    se = cursor.take(_VEC3)
    (ne_z,) = cursor.take(_F32)
    (sw_z,) = cursor.take(_F32)

    connections: list[int] = []
    for _ in range(NUM_DIRECTIONS):
        for _ in range(cursor.count()):
            connections.append(cursor.take(_U32)[0])

    (hiding_count,) = cursor.take(_U8)
    for _ in range(hiding_count):
        cursor.take(_HIDING_SPOT)

    for _ in range(cursor.count()):  # encounter paths
        *_head, spot_count = cursor.take(_ENCOUNTER_HEAD)
        for _ in range(spot_count):
            cursor.take(_ENCOUNTER_SPOT)

    cursor.take(_U16)  # place id

    ladder_links: list[int] = []
    for _ in range(NUM_LADDER_DIRECTIONS):
        for _ in range(cursor.count()):
            ladder_links.append(cursor.take(_U32)[0])

    cursor.take(_EARLIEST_TIMES)
    cursor.take(_LIGHT_INTENSITY)

    for _ in range(cursor.count()):  # visible areas
        cursor.take(_VISIBLE_AREA)
    cursor.take(_U32)  # inheritVisibilityFrom

    return NavArea(
        id=area_id,
        nw=tuple(nw),
        se=tuple(se),
        ne_z=ne_z,
        sw_z=sw_z,
        connections=tuple(connections),
        attributes=attributes,
        ladder_connections=tuple(ladder_links),
    )


def _validate(mesh: NavMesh, path: str) -> None:
    ids = {area.id for area in mesh.areas}
    if len(ids) != len(mesh.areas):
        raise NavReadError("nav_duplicate_id", "two areas share the same id", path)
    for area in mesh.areas:
        for target in area.connections + area.ladder_connections:
            if target not in ids:
                raise NavReadError(
                    "nav_bad_reference",
                    f"area {area.id} links to unknown area {target}",
                    path,
                )


# --- geometry -----------------------------------------------------------------


def _bounds(area: NavArea) -> tuple[float, float, float, float]:
    return (
        min(area.nw[0], area.se[0]),
        min(area.nw[1], area.se[1]),
        max(area.nw[0], area.se[0]),
        max(area.nw[1], area.se[1]),
    )


def _height_at(area: NavArea, x: float, y: float) -> float:
    """Bilinear surface height at (x, y), clamped to the area footprint."""

    min_x, min_y, max_x, max_y = _bounds(area)
    width = max_x - min_x
    depth = max_y - min_y
    u = min(max((x - min_x) / width, 0.0), 1.0) if width > 0 else 0.0
    v = min(max((y - min_y) / depth, 0.0), 1.0) if depth > 0 else 0.0
    nw_z = area.nw[2]
    se_z = area.se[2]
    return (
        (1 - u) * (1 - v) * nw_z
        + u * (1 - v) * area.ne_z
        + u * v * se_z
        + (1 - u) * v * area.sw_z
    )


def _contains_xy(area: NavArea, x: float, y: float) -> bool:
    min_x, min_y, max_x, max_y = _bounds(area)
    return min_x <= x <= max_x and min_y <= y <= max_y


def _center(area: NavArea) -> tuple[float, float, float]:
    min_x, min_y, max_x, max_y = _bounds(area)
    center_z = (area.nw[2] + area.se[2] + area.ne_z + area.sw_z) / NUM_CORNERS
    return ((min_x + max_x) / 2.0, (min_y + max_y) / 2.0, center_z)


def nearest_area(mesh: NavMesh, x: float, y: float, z: float | None = None) -> NearestArea:
    """Area containing (x, y), else the area with the closest center.

    When several areas contain (x, y) (shared edges), the one whose surface is
    closest to ``z`` wins; without ``z`` the lowest id wins.  Ties are broken
    by id so the result is deterministic.
    """

    if not mesh.areas:
        raise ValueError("nearest_area needs a mesh with at least one area")

    containing = [area for area in mesh.areas if _contains_xy(area, x, y)]
    if containing:
        if z is None:
            best = min(containing, key=lambda area: area.id)
        else:
            best = min(containing, key=lambda area: (abs(z - _height_at(area, x, y)), area.id))
        inside = True
    else:
        def center_key(area: NavArea) -> tuple[float, int]:
            cx, cy, cz = _center(area)
            dz = (z - cz) if z is not None else 0.0
            return ((x - cx) ** 2 + (y - cy) ** 2 + dz**2, area.id)

        best = min(mesh.areas, key=center_key)
        inside = False

    min_x, min_y, max_x, max_y = _bounds(best)
    clamped_x = min(max(x, min_x), max_x)
    clamped_y = min(max(y, min_y), max_y)
    dxy_sq = (x - clamped_x) ** 2 + (y - clamped_y) ** 2
    dz = (z - _height_at(best, clamped_x, clamped_y)) if z is not None else 0.0
    cx, cy, cz = _center(best)
    center_sq = (x - cx) ** 2 + (y - cy) ** 2 + ((z - cz) ** 2 if z is not None else 0.0)
    return NearestArea(
        area_id=best.id,
        inside=inside,
        distance=sqrt(dxy_sq + dz**2),
        center_distance=sqrt(center_sq),
    )


# --- graph --------------------------------------------------------------------


def _links(area: NavArea, include_ladders: bool) -> tuple[int, ...]:
    if include_ladders:
        return area.connections + area.ladder_connections
    return area.connections


def _directed_adjacency(mesh: NavMesh, include_ladders: bool) -> dict[int, tuple[int, ...]]:
    return {area.id: _links(area, include_ladders) for area in mesh.areas}


def _reach(adjacency: dict[int, tuple[int, ...]], start: int) -> frozenset[int]:
    seen = {start}
    queue = deque([start])
    while queue:
        current = queue.popleft()
        for target in adjacency[current]:
            if target not in seen:
                seen.add(target)
                queue.append(target)
    return frozenset(seen)


def _component_list(mesh: NavMesh, include_ladders: bool) -> tuple[tuple[int, ...], ...]:
    parent = {area.id: area.id for area in mesh.areas}

    def find(node: int) -> int:
        while parent[node] != node:
            parent[node] = parent[parent[node]]
            node = parent[node]
        return node

    for area in mesh.areas:
        for target in _links(area, include_ladders):
            root_a = find(area.id)
            root_b = find(target)
            if root_a != root_b:
                parent[max(root_a, root_b)] = min(root_a, root_b)

    groups: dict[int, list[int]] = {}
    for area in mesh.areas:
        groups.setdefault(find(area.id), []).append(area.id)
    result = [tuple(sorted(members)) for members in groups.values()]
    result.sort(key=lambda members: (-len(members), members[0]))
    return tuple(result)


def components(mesh: NavMesh, *, include_ladders: bool = False) -> tuple[tuple[int, ...], ...]:
    """Connected components with links treated as undirected.

    Sorted by size (largest first), then by lowest area id.
    """

    return _component_list(mesh, include_ladders)


def reachable_from(mesh: NavMesh, area_id: int, *, include_ladders: bool = False) -> frozenset[int]:
    """Area ids reachable from ``area_id`` following links in their stored direction."""

    adjacency = _directed_adjacency(mesh, include_ladders)
    if area_id not in adjacency:
        raise ValueError(f"area {area_id} is not in the mesh")
    return _reach(adjacency, area_id)


def check_points(
    mesh: NavMesh,
    points: dict[str, Vec3],
    *,
    include_ladders: bool = False,
) -> PointReport:
    """Locate named points on the mesh and test directed reachability for every pair.

    A pair is mutually reachable only if the directed reachability holds in both
    directions.  ``component`` is the undirected component of the nearest area;
    two points can share a component and still be one-way unreachable.
    """

    component_list = _component_list(mesh, include_ladders)
    component_of: dict[int, int] = {}
    for index, members in enumerate(component_list):
        for area_id in members:
            component_of[area_id] = index

    adjacency = _directed_adjacency(mesh, include_ladders)

    locations: dict[str, PointLocation] = {}
    for name, (x, y, z) in points.items():
        nearest = nearest_area(mesh, x, y, z)
        locations[name] = PointLocation(
            name=name,
            position=(x, y, z),
            area_id=nearest.area_id,
            component=component_of[nearest.area_id],
            inside=nearest.inside,
            distance=nearest.distance,
            center_distance=nearest.center_distance,
        )

    reach_cache: dict[int, frozenset[int]] = {}

    def reach(area_id: int) -> frozenset[int]:
        if area_id not in reach_cache:
            reach_cache[area_id] = _reach(adjacency, area_id)
        return reach_cache[area_id]

    names = list(points)
    pairs: list[PairReachability] = []
    for i, name_a in enumerate(names):
        for name_b in names[i + 1 :]:
            area_a = locations[name_a].area_id
            area_b = locations[name_b].area_id
            pairs.append(
                PairReachability(
                    a=name_a,
                    b=name_b,
                    a_to_b=area_b in reach(area_a),
                    b_to_a=area_a in reach(area_b),
                )
            )

    return PointReport(
        locations=locations,
        pairs=tuple(pairs),
        component_count=len(component_list),
        component_sizes=tuple(len(members) for members in component_list),
    )
