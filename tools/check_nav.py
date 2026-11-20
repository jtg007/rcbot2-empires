"""Check Source navigation mesh connectivity, optionally against BSP entity points.

Usage:
    python tools/check_nav.py <file.nav>
    python tools/check_nav.py <file.nav> --bsp <file.bsp>

Prints the area count, the component count and the largest components.  With
``--bsp`` the entity lump is read and the origins of spawn, commander,
barracks, refinery and resource entities are located on the mesh.  Reports
the component and the nearest-area distance of each point and every pair that
is not mutually reachable along directed links.

Exit code: 0 when every located point is on the main component or within
``--reach-hu`` (default 384 HU, a hypothesis covering resource rock props) of
it, 1 otherwise, 2 on read errors.  The strict pair list (by each point's own
area) is still printed for diagnosis.
Read-only: neither input file is modified.
"""

from __future__ import annotations

import argparse
import math
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

from navlib.bsp import BSPReadError  # noqa: E402
from navlib.bsp_entities import (  # noqa: E402
    read_bsp_entities,
    select_point_entities,
)
from navlib.navmesh import NavReadError, check_points, components, read_nav  # noqa: E402


DEFAULT_REACH_HU = 384.0


def describe_mesh(mesh, nav_path: Path) -> list[str]:
    comps = components(mesh)
    sizes = [len(members) for members in comps]
    singletons = sum(1 for size in sizes if size == 1)
    lines = [
        f"nav: {nav_path}",
        f"  version {mesh.version}, subversion {mesh.subversion}, ladders {mesh.ladder_count}",
        f"  areas: {len(mesh.areas)}",
        f"  components (undirected, ground links): {len(comps)}  singletons: {singletons}",
        "  largest components (size, share of areas):",
    ]
    for index, size in enumerate(sizes[:5]):
        lines.append(f"    #{index}: {size} areas ({size / len(mesh.areas):.2%})")
    return lines


def _horizontal_distance(area, x: float, y: float) -> float:
    dx = max(area.nw[0] - x, 0.0, x - area.se[0])
    dy = max(area.nw[1] - y, 0.0, y - area.se[1])
    return math.hypot(dx, dy)


def describe_points(mesh, bsp_path: Path, reach_hu: float = DEFAULT_REACH_HU) -> tuple[list[str], bool]:
    entities = read_bsp_entities(bsp_path)

    classname_counts: dict[str, int] = {}
    for entity in entities:
        name = entity.get("classname", "<none>")
        classname_counts[name] = classname_counts.get(name, 0) + 1

    lines = [f"bsp: {bsp_path}", f"  entities: {len(entities)}", "  classnames (count):"]
    for name in sorted(classname_counts):
        lines.append(f"    {name}: {classname_counts[name]}")

    point_entities, skipped = select_point_entities(entities)
    lines.extend(f"  {message}" for message in skipped)
    points = {point.name: point.origin for point in point_entities}

    if not points:
        lines.append("  no spawn/commander/building/resource entities with origins found")
        return lines, True

    report = check_points(mesh, points)
    lines.append(f"  located points: {len(points)}")
    lines.append("  point locations (name, origin, area, component, inside, distance, center distance):")
    for name, location in report.locations.items():
        x, y, z = location.position
        lines.append(
            f"    {name} origin=({x:.1f}, {y:.1f}, {z:.1f}) area={location.area_id} "
            f"component=#{location.component} inside={location.inside} "
            f"distance={location.distance:.1f} center_distance={location.center_distance:.1f}"
        )

    by_component: dict[int, list[str]] = {}
    for name, location in report.locations.items():
        by_component.setdefault(location.component, []).append(name)
    lines.append("  points per component:")
    for component in sorted(by_component):
        lines.append(f"    #{component}: {len(by_component[component])} points")
        for name in by_component[component]:
            lines.append(f"      {name}")

    # Verdict against the main component.  A point whose own area is an island
    # (typically the top of the resource rock prop) still counts as reachable
    # when a main-component area lies within ``reach_hu`` horizontally.
    main = set(components(mesh)[0]) if mesh.areas else set()
    main_areas = [area for area in mesh.areas if area.id in main]
    lines.append(f"  verdict against the main component (reach {reach_hu:.0f} HU):")
    unreachable: list[str] = []
    for name, location in report.locations.items():
        if location.area_id in main:
            status = "main"
        else:
            x, y, _ = location.position
            gap = min((_horizontal_distance(area, x, y) for area in main_areas), default=math.inf)
            if gap <= reach_hu:
                status = f"adjacent (own area is an island, main mesh {gap:.0f} HU away)"
            else:
                status = f"UNREACHABLE (main mesh {gap:.0f} HU away)"
                unreachable.append(name)
        if status != "main":
            lines.append(f"    {name}: {status}")
    lines.append(f"  unreachable points: {len(unreachable)}")

    failing = [pair for pair in report.pairs if not pair.both]
    lines.append(f"  pairs (strict, by own area): {len(report.pairs)}  not mutually reachable: {len(failing)}")
    for pair in failing:
        a_loc = report.locations[pair.a]
        b_loc = report.locations[pair.b]
        if not pair.a_to_b and not pair.b_to_a:
            direction = "neither direction"
        elif pair.a_to_b:
            direction = f"{pair.a} -> {pair.b} only"
        else:
            direction = f"{pair.b} -> {pair.a} only"
        lines.append(
            f"    {pair.a} (area {a_loc.area_id}, comp #{a_loc.component}) <-> "
            f"{pair.b} (area {b_loc.area_id}, comp #{b_loc.component}): {direction}"
        )
    return lines, not unreachable


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("nav", type=Path, help="Source .nav file")
    parser.add_argument("--bsp", type=Path, default=None, help="compiled BSP for entity origins")
    parser.add_argument(
        "--reach-hu", type=float, default=DEFAULT_REACH_HU,
        help="horizontal distance to the main component that still counts as reachable",
    )
    args = parser.parse_args(argv)

    try:
        mesh = read_nav(args.nav)
        lines = describe_mesh(mesh, args.nav)
        ok = True
        if args.bsp is not None:
            point_lines, ok = describe_points(mesh, args.bsp, args.reach_hu)
            lines.extend(point_lines)
    except (NavReadError, BSPReadError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    print("\n".join(lines))
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
