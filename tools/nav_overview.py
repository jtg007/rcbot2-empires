"""Render a top-down PNG of a Source navigation mesh for visual connectivity review.

Usage:
    python3 -I tools/nav_overview.py <file.nav> --output out.png [--bsp file.bsp]
        [--size 2048] [--json out.json] [--marks marks.json]
        [--crop x0,y0,x1,y1] [--terrain] [--vehicle-width 284]
        [--log server.log [--userid N]]

Project additions (RCBot2 Empires): ``--crop`` zooms to a world rectangle (widened
to the aspect of the map panel) with an adaptive grid; ``--terrain`` (needs ``--bsp``) shades the displacement terrain under
the mesh (grey = height, amber 30-45 deg, red > 45 deg slope); ``--vehicle-width W``
tints mesh that a vehicle of width W cannot pass (open the mesh by W/2, square
kernel, so diagonals are slightly pessimistic); ``--log`` turns the stuck/route-block
lines of a server.log into marks and, with ``--userid``, draws that vehicle's
``vehicle drive`` track as a line.

Every nav area is drawn as a filled rectangle.  The largest connected component
(undirected ground links) is gray-green, lighter where the mean height is higher.
Other components get a palette colour; components with at most three areas are
magenta.  With ``--bsp`` the commander, barracks, refinery, resource and spawn
entities are drawn; points outside the largest component are labelled with
their component id in red.  ``emp_resource_point_prop`` entities share their
origin with ``emp_resource_point`` and are only listed in the JSON.

``--marks`` takes a JSON list of ``{"x", "y", "label", "color"}`` objects
(``color`` is ``"#rrggbb"``, optional) that are drawn as X markers, for example
bot stuck or failure locations.

The image has a legend panel to the right of the map.  Output is deterministic:
the same input and options produce the same PNG bytes (no timestamps).  The
fonts are the first DejaVu/Liberation TrueType files found on the system, so
bytes are only guaranteed to match on the same machine.

Read-only: input files are never modified.  Exit code 0 on success, 2 on
read or argument errors.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import math
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont  # noqa: E402

from navlib.bsp import BSPReadError  # noqa: E402
from navlib.bsp_entities import PointEntity, read_bsp_entities, select_point_entities  # noqa: E402
from navlib.navmesh import NavMesh, NavReadError, check_points, components, read_nav  # noqa: E402
from navlib.static_props import StaticProp, read_static_props  # noqa: E402
from navlib.terrain import read_displacement_triangles, slope_degrees  # noqa: E402

Color = tuple[int, int, int]

GRID_STEPS = (32.0, 64.0, 128.0, 256.0, 512.0, 1024.0, 2048.0, 4096.0)
SMALL_ISLAND_MAX = 3
PANEL_WIDTH = 460
MAP_LEFT = 110  # room for y-axis labels
MAP_TOP = 20
MAP_RIGHT_PAD = 20
MAP_BOTTOM = 100  # room for x-axis labels
BACKGROUND: Color = (22, 24, 28)
PANEL_BACKGROUND: Color = (34, 37, 44)
TEXT: Color = (235, 235, 235)
MUTED: Color = (170, 175, 185)
RED_TEXT: Color = (255, 80, 80)
MAGENTA: Color = (255, 0, 255)
MAIN_DARK: Color = (58, 84, 62)
MAIN_LIGHT: Color = (182, 210, 164)
BE_COLOR: Color = (40, 120, 255)
NF_COLOR: Color = (235, 40, 40)
RESOURCE_COLOR: Color = (255, 215, 0)
NEUTRAL_COLOR: Color = (200, 200, 200)
MARK_DEFAULT: Color = (255, 140, 0)
TRACK_COLOR: Color = (0, 255, 255)
NARROW_COLOR: Color = (255, 90, 170)
PROP_COLOR: Color = (40, 200, 60)
LOG_KINDS = (  # (regex searched in the line, short label, colour, position key)
    ("cv-drive stuck", "cv-stuck", (255, 0, 0), "pos"),
    ("stuck-reverse", "reverse", (255, 120, 0), "pos"),
    ("stuck-back-turn", "back-turn", (255, 200, 0), "pos"),
    ("route-block", "route-block", (255, 0, 255), "pos"),
    ("bail-out", "bail-out", (255, 60, 60), "pos"),
    ("navmesh-stuck point", "nav-stuck", (255, 255, 255), "bot"),
)
PALETTE: tuple[Color, ...] = (
    (255, 140, 0),
    (0, 200, 255),
    (150, 100, 255),
    (120, 230, 40),
    (0, 170, 130),
    (255, 230, 90),
    (160, 100, 50),
)

_FONT_FILES = {
    False: ("/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/liberation/LiberationSans-Regular.ttf"),
    True: ("/usr/share/fonts/TTF/DejaVuSans-Bold.ttf", "/usr/share/fonts/liberation/LiberationSans-Bold.ttf"),
}


@dataclass(frozen=True)
class Mark:
    x: float
    y: float
    label: str
    color: Color


def load_marks(path: Path) -> list[Mark]:
    """Parse a marks file: a JSON list of ``{"x", "y", "label", "color"}`` objects."""

    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, list):
        raise ValueError(f"marks file {path} must contain a JSON list")
    marks: list[Mark] = []
    for index, item in enumerate(data):
        if not isinstance(item, dict):
            raise ValueError(f"mark #{index} is not an object")
        try:
            x = float(item["x"])
            y = float(item["y"])
        except (KeyError, TypeError, ValueError) as error:
            raise ValueError(f"mark #{index} needs numeric x and y") from error
        label = str(item.get("label", ""))
        color = _parse_color(item.get("color"), index)
        marks.append(Mark(x=x, y=y, label=label, color=color))
    return marks


def _parse_color(raw: object, index: int) -> Color:
    if raw is None:
        return MARK_DEFAULT
    if not isinstance(raw, str) or len(raw) != 7 or not raw.startswith("#"):
        raise ValueError(f"mark #{index} color must be '#rrggbb'")
    try:
        return (int(raw[1:3], 16), int(raw[3:5], 16), int(raw[5:7], 16))
    except ValueError as error:
        raise ValueError(f"mark #{index} color must be '#rrggbb'") from error


def _kind_of(classname: str) -> str:
    if classname.endswith("commander"):
        return "commander"
    if "barracks" in classname:
        return "barracks"
    if "refinery" in classname:
        return "refinery"
    if classname.startswith("emp_resource_point"):
        return "resource"
    if classname.startswith("emp_info_player_"):
        return "spawn"
    return "other"


def _side_of(classname: str) -> str | None:
    tokens = classname.split("_")
    if "imp" in tokens:
        return "BE"
    if "nf" in tokens:
        return "NF"
    return None


def _side_color(side: str | None) -> Color:
    if side == "BE":
        return BE_COLOR
    if side == "NF":
        return NF_COLOR
    return NEUTRAL_COLOR


@dataclass(frozen=True)
class Summary:
    map_name: str
    area_count: int
    comps: tuple[tuple[int, ...], ...]
    point_rows: tuple[dict, ...]
    marks: tuple[Mark, ...]
    bounds: tuple[float, float, float, float]  # min x, min y, max x, max y
    tracks: tuple[tuple[tuple[float, float], ...], ...] = ()


def build_summary(
    mesh: NavMesh,
    map_name: str,
    point_entities: tuple[PointEntity, ...],
    marks: tuple[Mark, ...] = (),
    tracks: tuple[tuple[tuple[float, float], ...], ...] = (),
    crop: tuple[float, float, float, float] | None = None,
) -> Summary:
    comps = components(mesh)

    points = {point.name: point.origin for point in point_entities}
    report = check_points(mesh, points) if points else None
    by_name = {point.name: point for point in point_entities}

    rows: list[dict] = []
    if report is not None:
        for name in sorted(report.locations, key=lambda key: (by_name[key].classname, by_name[key].origin)):
            location = report.locations[name]
            entity = by_name[name]
            rows.append(
                {
                    "name": name,
                    "classname": entity.classname,
                    "targetname": entity.targetname,
                    "kind": _kind_of(entity.classname),
                    "side": _side_of(entity.classname),
                    "x": location.position[0],
                    "y": location.position[1],
                    "z": location.position[2],
                    "area_id": location.area_id,
                    "component": location.component,
                    "distance": round(location.distance, 3),
                    "off_main": location.component != 0,
                    "is_prop": entity.classname.endswith("_prop"),
                }
            )

    min_x = min(min(a.nw[0], a.se[0]) for a in mesh.areas)
    min_y = min(min(a.nw[1], a.se[1]) for a in mesh.areas)
    max_x = max(max(a.nw[0], a.se[0]) for a in mesh.areas)
    max_y = max(max(a.nw[1], a.se[1]) for a in mesh.areas)
    for row in rows:
        min_x, min_y = min(min_x, row["x"]), min(min_y, row["y"])
        max_x, max_y = max(max_x, row["x"]), max(max_y, row["y"])
    for mark in marks:
        min_x, min_y = min(min_x, mark.x), min(min_y, mark.y)
        max_x, max_y = max(max_x, mark.x), max(max_y, mark.y)

    return Summary(
        map_name=map_name,
        area_count=len(mesh.areas),
        comps=comps,
        point_rows=tuple(rows),
        marks=marks,
        bounds=crop if crop is not None else (min_x, min_y, max_x, max_y),
        tracks=tracks,
    )


def summary_to_json(summary: Summary, nav_path: Path, bsp_path: Path | None) -> dict:
    main_size = len(summary.comps[0]) if summary.comps else 0
    sizes = [len(members) for members in summary.comps]
    return {
        "nav": str(nav_path),
        "bsp": str(bsp_path) if bsp_path is not None else None,
        "map": summary.map_name,
        "areas": summary.area_count,
        "components": {
            "count": len(summary.comps),
            "largest_areas": main_size,
            "largest_share": round(main_size / summary.area_count, 6) if summary.area_count else 0.0,
            "small_islands": sum(1 for size in sizes if size <= SMALL_ISLAND_MAX),
            "sizes_top20": sizes[:20],
        },
        "bounds": {
            "min": [summary.bounds[0], summary.bounds[1]],
            "max": [summary.bounds[2], summary.bounds[3]],
        },
        "points": [
            {
                "name": row["name"],
                "classname": row["classname"],
                "targetname": row["targetname"],
                "kind": row["kind"],
                "side": row["side"],
                "x": row["x"],
                "y": row["y"],
                "z": row["z"],
                "area_id": row["area_id"],
                "component": row["component"],
                "distance": row["distance"],
                "off_main": row["off_main"],
                "is_prop": row["is_prop"],
            }
            for row in summary.point_rows
        ],
        "not_in_main_component": [row["name"] for row in summary.point_rows if row["off_main"]],
        "marks": [
            {"x": mark.x, "y": mark.y, "label": mark.label, "color": "#%02x%02x%02x" % mark.color}
            for mark in summary.marks
        ],
    }


class _View:
    """Maps world x/y to pixels; +Y is up in the world, so the image y is flipped."""

    def __init__(self, bounds: tuple[float, float, float, float], width: float, height: float, left: float, top: float):
        min_x, min_y, max_x, max_y = bounds
        self.min_x = min_x
        self.max_y = max_y
        world_w = max(max_x - min_x, 1.0)
        world_h = max(max_y - min_y, 1.0)
        self.scale = min(width / world_w, height / world_h)
        self.draw_w = world_w * self.scale
        self.draw_h = world_h * self.scale
        self.left = left + (width - self.draw_w) / 2.0
        self.top = top + (height - self.draw_h) / 2.0
        self.min_y = min_y

    def px(self, x: float) -> int:
        return int(round(self.left + (x - self.min_x) * self.scale))

    def py(self, y: float) -> int:
        return int(round(self.top + (self.max_y - y) * self.scale))


def _font(size: int, bold: bool = False) -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
    for candidate in _FONT_FILES[bold]:
        if Path(candidate).exists():
            return ImageFont.truetype(candidate, size)
    return ImageFont.load_default(size=size)


def _fit(draw: ImageDraw.ImageDraw, text: str, font, max_width: float) -> str:
    if draw.textlength(text, font=font) <= max_width:
        return text
    while text and draw.textlength(text + "...", font=font) > max_width:
        text = text[:-1]
    return text + "..."


def _area_colors(mesh: NavMesh, comps: tuple[tuple[int, ...], ...]) -> dict[int, Color]:
    """Fill colour per area id."""

    by_id = {area.id: area for area in mesh.areas}
    colors: dict[int, Color] = {}
    main = comps[0] if comps else ()
    heights = {area_id: (by_id[area_id].nw[2] + by_id[area_id].se[2] + by_id[area_id].ne_z + by_id[area_id].sw_z) / 4.0 for area_id in main}
    if heights:
        low = min(heights.values())
        high = max(heights.values())
        span = high - low
        for area_id, height in heights.items():
            t = (height - low) / span if span > 0 else 0.5
            colors[area_id] = tuple(
                int(round(dark + (light - dark) * t)) for dark, light in zip(MAIN_DARK, MAIN_LIGHT)
            )

    palette_index = 0
    for index, members in enumerate(comps):
        if index == 0:
            continue
        if len(members) <= SMALL_ISLAND_MAX:
            color = MAGENTA
        else:
            color = PALETTE[palette_index % len(PALETTE)]
            palette_index += 1
        for area_id in members:
            colors[area_id] = color
    return colors


def _darker(color: Color, factor: float = 0.45) -> Color:
    return tuple(int(channel * factor) for channel in color)


def _draw_symbol(draw: ImageDraw.ImageDraw, kind: str, x: int, y: int, color: Color, radius: int) -> None:
    outline = (0, 0, 0)
    if kind == "commander":
        draw.rectangle([x - radius, y - radius, x + radius, y + radius], fill=color, outline=(255, 255, 255), width=2)
    elif kind == "barracks":
        triangle = [(x, y - radius), (x - radius, y + radius * 0.8), (x + radius, y + radius * 0.8)]
        draw.polygon(triangle, fill=color, outline=(255, 255, 255))
    elif kind == "refinery":
        draw.ellipse([x - radius, y - radius, x + radius, y + radius], fill=color, outline=(255, 255, 255), width=2)
    elif kind == "resource":
        diamond = [(x, y - radius), (x + radius, y), (x, y + radius), (x - radius, y)]
        draw.polygon(diamond, fill=color, outline=outline)
    elif kind == "spawn":
        small = max(radius // 2, 3)
        draw.ellipse([x - small, y - small, x + small, y + small], fill=color, outline=outline)
    else:
        draw.ellipse([x - radius // 2, y - radius // 2, x + radius // 2, y + radius // 2], fill=color, outline=outline)


def _draw_mark(draw: ImageDraw.ImageDraw, x: int, y: int, color: Color, radius: int = 12) -> None:
    draw.line([(x - radius, y - radius), (x + radius, y + radius)], fill=color, width=4)
    draw.line([(x - radius, y + radius), (x + radius, y - radius)], fill=color, width=4)


def _terrain_color(tri, low: float, high: float) -> Color:
    """Grey by height, amber for 30-45 deg slope, red above 45 deg (not walkable for infantry)."""

    slope = slope_degrees(tri)
    z = (tri[0][2] + tri[1][2] + tri[2][2]) / 3.0
    t = (z - low) / (high - low) if high > low else 0.5
    grey = int(40 + 90 * min(max(t, 0.0), 1.0))
    if slope > 45.0:
        return (170, 40, 40)
    if slope > 30.0:
        return (170, 120, 40)
    return (grey, grey, grey + 6)


def _draw_terrain(image: Image.Image, view: "_View", triangles, bounds) -> None:
    x0, y0, x1, y1 = bounds
    visible = [
        t for t in triangles
        if max(p[0] for p in t) >= x0 and min(p[0] for p in t) <= x1 and max(p[1] for p in t) >= y0 and min(p[1] for p in t) <= y1
    ]
    if not visible:
        return
    low = min(p[2] for t in visible for p in t)
    high = max(p[2] for t in visible for p in t)
    draw = ImageDraw.Draw(image)
    # Painter order: low surfaces first, so cliffs and higher floors cover what lies below them.
    for tri in sorted(visible, key=lambda t: t[0][2] + t[1][2] + t[2][2]):
        draw.polygon([(view.px(p[0]), view.py(p[1])) for p in tri], fill=_terrain_color(tri, low, high))


def _narrow_mask(image_size: tuple[int, int], mesh: NavMesh, view: "_View", bounds, vehicle_width: float) -> Image.Image:
    """Mesh pixels that survive no opening by half the vehicle width (255 = too narrow)."""

    mask = Image.new("L", image_size, 0)
    draw = ImageDraw.Draw(mask)
    # Areas just outside the view still widen the mesh at the view edge.
    x0, y0, x1, y1 = bounds[0] - vehicle_width, bounds[1] - vehicle_width, bounds[2] + vehicle_width, bounds[3] + vehicle_width
    for area in mesh.areas:
        ax0, ax1 = sorted((area.nw[0], area.se[0]))
        ay0, ay1 = sorted((area.nw[1], area.se[1]))
        if ax1 < x0 or ax0 > x1 or ay1 < y0 or ay0 > y1:
            continue
        draw.rectangle([view.px(ax0), view.py(ay1), max(view.px(ax1), view.px(ax0)), max(view.py(ay0), view.py(ay1))], fill=255)
    steps = max(1, int(round(vehicle_width / 2.0 * view.scale)))
    opened = mask
    for _ in range(steps):
        opened = opened.filter(ImageFilter.MinFilter(3))
    for _ in range(steps):
        opened = opened.filter(ImageFilter.MaxFilter(3))
    return ImageChops.subtract(mask, opened)


def _grid_step(span: float) -> float:
    for step in GRID_STEPS:
        if span / step <= 12:
            return step
    return GRID_STEPS[-1]


def render_overview(
    mesh: NavMesh,
    summary: Summary,
    size: int,
    triangles=None,
    vehicle_width: float = 0.0,
    props: tuple[StaticProp, ...] = (),
) -> Image.Image:
    """Render the map and legend panel.  The image is ``size`` high and ``size + PANEL_WIDTH`` wide."""

    if size < 400:
        raise ValueError("--size must be at least 400")
    width = size + PANEL_WIDTH
    image = Image.new("RGBA", (width, size), BACKGROUND + (255,))
    draw = ImageDraw.Draw(image)

    map_width = size - MAP_LEFT - MAP_RIGHT_PAD
    map_height = size - MAP_TOP - MAP_BOTTOM
    view = _View(summary.bounds, map_width, map_height, MAP_LEFT, MAP_TOP)
    colors = _area_colors(mesh, summary.comps)

    min_x, min_y, max_x, max_y = summary.bounds
    if triangles:
        _draw_terrain(image, view, triangles, summary.bounds)

    # With terrain underneath, the mesh is drawn as a translucent layer so the ground stays visible.
    layer = Image.new("RGBA", image.size, (0, 0, 0, 0)) if triangles else None
    area_draw = ImageDraw.Draw(layer) if layer is not None else draw
    alpha = 110 if triangles else 255
    for area in mesh.areas:
        x0, x1 = sorted((area.nw[0], area.se[0]))
        y0, y1 = sorted((area.nw[1], area.se[1]))
        if x1 < min_x or x0 > max_x or y1 < min_y or y0 > max_y:
            continue
        left, right = view.px(x0), view.px(x1)
        top, bottom = view.py(y1), view.py(y0)
        right = max(right, left)
        bottom = max(bottom, top)
        fill = colors.get(area.id, MAGENTA)
        outline = _darker(fill) if (right - left) >= 4 and (bottom - top) >= 4 else None
        area_draw.rectangle([left, top, right, bottom], fill=fill + (alpha,), outline=outline)
    if layer is not None:
        image = Image.alpha_composite(image, layer)
        draw = ImageDraw.Draw(image)

    if vehicle_width > 0.0:
        narrow = _narrow_mask(image.size, mesh, view, summary.bounds, vehicle_width)
        image.paste(Image.new("RGBA", image.size, NARROW_COLOR + (255,)), (0, 0), narrow.point(lambda v: 170 if v else 0))
        draw = ImageDraw.Draw(image)

    # Keep the margins clean: terrain, areas and the narrow mask may reach beyond the map rectangle.
    draw.rectangle([0, 0, MAP_LEFT - 1, size], fill=BACKGROUND)
    draw.rectangle([0, 0, size, MAP_TOP - 1], fill=BACKGROUND)
    draw.rectangle([0, size - MAP_BOTTOM + 1, size, size], fill=BACKGROUND)
    draw.rectangle([size - MAP_RIGHT_PAD + 1, 0, size, size], fill=BACKGROUND)

    # Static props (palms, rocks): solid in game, unknown to the mesh. Solid ones white-ringed, others grey.
    for prop in props:
        if not (min_x <= prop.x <= max_x and min_y <= prop.y <= max_y):
            continue
        px, py = view.px(prop.x), view.py(prop.y)
        r = max(4, int(view.px(prop.x + 32.0) - px))
        fill = PROP_COLOR if prop.solid else (120, 120, 120)
        draw.ellipse([px - r, py - r, px + r, py + r], fill=fill, outline=(255, 255, 255) if prop.solid else None, width=2)

    for track in summary.tracks:
        points = [(view.px(x), view.py(y)) for x, y in track]
        if len(points) >= 2:
            draw.line(points, fill=TRACK_COLOR, width=3)
        for index, point in enumerate(points):
            draw.ellipse([point[0] - 4, point[1] - 4, point[0] + 4, point[1] + 4], outline=TRACK_COLOR)
            if index == 0:
                draw.text((point[0] + 8, point[1] - 8), "start", font=_font(18), fill=TRACK_COLOR, stroke_width=2, stroke_fill=(0, 0, 0))

    grid = Image.new("RGBA", image.size, (0, 0, 0, 0))
    grid_draw = ImageDraw.Draw(grid)
    label_font = _font(20)
    grid_color = (255, 255, 255, 60)
    step = _grid_step(max(max_x - min_x, max_y - min_y))
    gx = math.ceil(min_x / step) * step
    while gx <= max_x:
        px = view.px(gx)
        grid_draw.line([(px, MAP_TOP), (px, size - MAP_BOTTOM)], fill=grid_color, width=1)
        draw.text((px, size - MAP_BOTTOM + 8), f"{gx:.0f}", font=label_font, fill=MUTED, anchor="mt")
        gx += step
    gy = math.ceil(min_y / step) * step
    while gy <= max_y:
        py = view.py(gy)
        grid_draw.line([(MAP_LEFT, py), (size - MAP_RIGHT_PAD, py)], fill=grid_color, width=1)
        draw.text((MAP_LEFT - 8, py), f"{gy:.0f}", font=label_font, fill=MUTED, anchor="rm")
        gy += step
    draw.text((MAP_LEFT, size - 40), "x (HU) ->", font=label_font, fill=MUTED, anchor="lm")
    draw.text((MAP_LEFT - 8, MAP_TOP + 10), "y (HU)", font=label_font, fill=MUTED, anchor="rm")

    image = Image.alpha_composite(image, grid)
    draw = ImageDraw.Draw(image)

    radius = max(10, min(16, size // 150))
    text_font = _font(max(18, size // 110))
    text_bold = _font(max(18, size // 110), bold=True)
    for row in summary.point_rows:
        if row["is_prop"]:
            continue
        px, py = view.px(row["x"]), view.py(row["y"])
        _draw_symbol(draw, row["kind"], px, py, _side_color(row["side"]), radius)
        lines: list[tuple[str, Color]] = []
        if row["kind"] == "resource":
            lines.append((row["targetname"] or row["classname"], TEXT))
        elif row["off_main"] and row["targetname"]:
            lines.append((row["targetname"], TEXT))
        if row["off_main"]:
            lines.append((f"comp #{row['component']}", RED_TEXT))
        for line_index, (text, fill) in enumerate(lines):
            draw.text(
                (px + radius + 6, py - 10 + line_index * (text_font.size + 2)),
                text,
                font=text_bold if fill == RED_TEXT else text_font,
                fill=fill,
                stroke_width=3,
                stroke_fill=(0, 0, 0),
                anchor="lt",
            )

    for mark in summary.marks:
        px, py = view.px(mark.x), view.py(mark.y)
        _draw_mark(draw, px, py, mark.color)
        if mark.label:
            draw.text(
                (px + 16, py - 10),
                mark.label,
                font=text_font,
                fill=mark.color,
                stroke_width=3,
                stroke_fill=(0, 0, 0),
                anchor="lt",
            )

    _draw_legend(draw, summary, size, radius, step, triangles is not None, vehicle_width, bool(props))
    return image.convert("RGB")


def _draw_legend(
    draw: ImageDraw.ImageDraw, summary: Summary, size: int, radius: int, step: float, terrain: bool, vehicle_width: float,
    props: bool = False,
) -> None:
    x0 = size
    draw.rectangle([x0, 0, x0 + PANEL_WIDTH - 1, size - 1], fill=PANEL_BACKGROUND)
    pad = 20
    left = x0 + pad
    max_width = PANEL_WIDTH - 2 * pad
    body = _font(22)
    small = _font(19)
    bold = _font(24, bold=True)
    title = _font(30, bold=True)
    bottom_limit = size - pad

    main_size = len(summary.comps[0]) if summary.comps else 0
    share = main_size / summary.area_count if summary.area_count else 0.0
    small_islands = sum(1 for members in summary.comps if len(members) <= SMALL_ISLAND_MAX)
    off_main = [row for row in summary.point_rows if row["off_main"]]

    y = pad
    draw.text((left, y), "Navigation overview", font=title, fill=TEXT)
    y += 46
    stats = [
        f"Map: {summary.map_name}",
        f"Areas: {summary.area_count}",
        f"Components: {len(summary.comps)} (ground links)",
        f"Largest: {main_size} areas ({share:.2%})",
        f"Islands with <= {SMALL_ISLAND_MAX} areas: {small_islands}",
        f"Entities: {len(summary.point_rows)}, outside main: {len(off_main)}",
    ]
    for line in stats:
        draw.text((left, y), _fit(draw, line, body, max_width), font=body, fill=TEXT)
        y += 30

    y += 14
    draw.text((left, y), "Key", font=bold, fill=TEXT)
    y += 36
    key_rows = [
        ("swatch", (MAIN_LIGHT, "Main component (lighter = higher)")),
        ("swatch", (PALETTE[0], "Other component (one colour each)")),
        ("swatch", (MAGENTA, f"Island with <= {SMALL_ISLAND_MAX} areas")),
        ("commander", (BE_COLOR, "Commander: square (BE blue, NF red)")),
        ("barracks", (BE_COLOR, "Barracks: triangle")),
        ("refinery", (BE_COLOR, "Refinery: circle")),
        ("resource", (RESOURCE_COLOR, "Resource point: diamond")),
        ("spawn", (NEUTRAL_COLOR, "Spawn (emp_info_player_*): dot")),
        ("mark", (MARK_DEFAULT, "Mark from --marks: X")),
        ("text", (MUTED, f"Grid every {step:.0f} HU")),
    ]
    if terrain:
        key_rows.insert(0, ("swatch", ((170, 40, 40), "Terrain slope > 45 deg")))
        key_rows.insert(1, ("swatch", ((170, 120, 40), "Terrain slope 30-45 deg")))
    if vehicle_width > 0.0:
        key_rows.insert(0, ("swatch", (NARROW_COLOR, f"Mesh too narrow for width {vehicle_width:.0f}")))
    if summary.tracks:
        key_rows.append(("swatch", (TRACK_COLOR, "Vehicle track from --log")))
    if props:
        key_rows.append(("swatch", (PROP_COLOR, "Static prop (--props), 32 HU circle")))
    for kind, (color, text) in key_rows:
        icon_x = left + 14
        icon_y = y + 13
        if kind == "swatch":
            draw.rectangle([left, y + 2, left + 28, y + 24], fill=color)
        elif kind == "mark":
            _draw_mark(draw, icon_x, icon_y, color, radius=9)
        elif kind == "text":
            draw.line([(left, icon_y), (left + 28, icon_y)], fill=MUTED, width=1)
        else:
            _draw_symbol(draw, kind, icon_x, icon_y, color, 10)
        draw.text((left + 40, y + 2), _fit(draw, text, small, max_width - 40), font=small, fill=TEXT)
        y += 28

    y += 14
    draw.text((left, y), "Entities not in main component", font=bold, fill=TEXT)
    y += 36
    if not off_main:
        draw.text((left, y), "none", font=body, fill=MUTED)
        y += 30
    line_height = 26
    for index, row in enumerate(off_main):
        if y + line_height > bottom_limit:
            draw.text((left, y), f"... +{len(off_main) - index} more", font=body, fill=MUTED)
            y += line_height
            break
        text = f"{row['targetname'] or row['classname']}  comp #{row['component']}"
        draw.text((left, y), _fit(draw, text, small, max_width), font=small, fill=RED_TEXT)
        y += line_height

    if summary.marks and y + 60 < bottom_limit:
        y += 14
        draw.text((left, y), f"Marks: {len(summary.marks)}", font=bold, fill=TEXT)
        y += 36
        for index, mark in enumerate(summary.marks):
            if y + line_height > bottom_limit:
                draw.text((left, y), f"... +{len(summary.marks) - index} more", font=body, fill=MUTED)
                break
            text = mark.label or f"({mark.x:.0f}, {mark.y:.0f})"
            draw.text((left, y), _fit(draw, text, small, max_width), font=small, fill=mark.color)
            y += line_height


_POS_RE = {key: re.compile(key + r"=\((-?\d+(?:\.\d+)?) (-?\d+(?:\.\d+)?) (-?\d+(?:\.\d+)?)\)") for key in ("pos", "bot")}
_USERID_RE = re.compile(r"userid=(\d+)")


def marks_from_log(path: Path, userid: int | None, max_marks: int = 60) -> tuple[list[Mark], list[tuple[float, float]]]:
    """Stuck/route-block events and one vehicle's ``vehicle drive`` track from a bot server.log.

    Events of the same kind within 150 HU are merged into one mark ``kind xN``.
    ``userid`` limits the events (except ``navmesh-stuck``, which has no userid) and picks the track.
    """

    merged: list[list] = []  # [x, y, label, color, count]
    track: list[tuple[float, float]] = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        owner = _USERID_RE.search(line)
        if userid is not None and "vehicle drive userid=" in line and owner and int(owner.group(1)) == userid:
            match = _POS_RE["pos"].search(line)
            if match:
                track.append((float(match.group(1)), float(match.group(2))))
            continue
        for needle, label, color, key in LOG_KINDS:
            if needle not in line:
                continue
            if userid is not None and owner and int(owner.group(1)) != userid:
                break
            match = _POS_RE[key].search(line)
            if not match:
                break
            x, y = float(match.group(1)), float(match.group(2))
            for entry in merged:
                if entry[2] == label and math.hypot(entry[0] - x, entry[1] - y) < 150.0:
                    entry[4] += 1
                    break
            else:
                merged.append([x, y, label, color, 1])
            break
    marks = [Mark(x, y, label if count == 1 else f"{label} x{count}", color) for x, y, label, color, count in merged]
    return marks[:max_marks], track


def _parse_crop(text: str) -> tuple[float, float, float, float]:
    try:
        x0, y0, x1, y1 = (float(part) for part in text.split(","))
    except ValueError as error:
        raise ValueError("--crop needs x0,y0,x1,y1") from error
    if x1 <= x0 or y1 <= y0:
        raise ValueError("--crop needs x1 > x0 and y1 > y0")
    return x0, y0, x1, y1


def _fit_crop_to_map(crop: tuple[float, float, float, float], size: int) -> tuple[float, float, float, float]:
    """Widen the crop to the aspect of the map rectangle, so everything drawn there lies inside the bounds."""

    if size < 400:
        raise ValueError("--size must be at least 400")
    x0, y0, x1, y1 = crop
    aspect = (size - MAP_LEFT - MAP_RIGHT_PAD) / (size - MAP_TOP - MAP_BOTTOM)
    w, h = x1 - x0, y1 - y0
    if w / h < aspect:
        grow = (h * aspect - w) / 2.0
        return x0 - grow, y0, x1 + grow, y1
    grow = (w / aspect - h) / 2.0
    return x0, y0 - grow, x1, y1 + grow


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("nav", type=Path, help="Source .nav file")
    parser.add_argument("--bsp", type=Path, default=None, help="compiled BSP for entity points")
    parser.add_argument("--output", type=Path, required=True, help="PNG file to write")
    parser.add_argument("--size", type=int, default=2048, help="map height in pixels (default 2048)")
    parser.add_argument("--json", type=Path, default=None, help="write the machine-readable summary here")
    parser.add_argument("--marks", type=Path, default=None, help="JSON list of marks to overlay")
    parser.add_argument("--crop", default=None, help="zoom to the world rectangle x0,y0,x1,y1 (HU)")
    parser.add_argument("--terrain", action="store_true", help="shade displacement terrain from --bsp under the mesh")
    parser.add_argument("--props", action="store_true", help="draw static props (palms, rocks) from --bsp; solid ones white-ringed")
    parser.add_argument("--vehicle-width", type=float, default=0.0, help="tint mesh narrower than this width (HU), e.g. 284")
    parser.add_argument("--log", type=Path, default=None, help="server.log: stuck/route-block events become marks")
    parser.add_argument("--userid", type=int, default=None, help="with --log: only this bot's events, plus its vehicle track")
    args = parser.parse_args(argv)

    try:
        mesh = read_nav(args.nav)
        if not mesh.areas:
            raise ValueError(f"{args.nav} contains no navigation areas")
        point_entities: tuple[PointEntity, ...] = ()
        if args.bsp is not None:
            point_entities, _skipped = select_point_entities(read_bsp_entities(args.bsp))
        marks = list(load_marks(args.marks)) if args.marks is not None else []
        tracks: list[tuple[tuple[float, float], ...]] = []
        if args.log is not None:
            log_marks, track = marks_from_log(args.log, args.userid)
            marks.extend(log_marks)
            if track:
                tracks.append(tuple(track))
        if args.terrain and args.bsp is None:
            raise ValueError("--terrain needs --bsp")
        if args.props and args.bsp is None:
            raise ValueError("--props needs --bsp")
        crop = _fit_crop_to_map(_parse_crop(args.crop), args.size) if args.crop else None
        summary = build_summary(mesh, args.nav.stem, point_entities, tuple(marks), tuple(tracks), crop)
        triangles = read_displacement_triangles(args.bsp, crop) if args.terrain else None
        props = read_static_props(args.bsp) if args.props else ()
        image = render_overview(mesh, summary, args.size, triangles, args.vehicle_width, props)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        image.save(args.output, format="PNG")
        if args.json is not None:
            data = summary_to_json(summary, args.nav, args.bsp)
            args.json.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    except (NavReadError, BSPReadError, OSError, ValueError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    print(f"wrote {args.output}")
    if args.json is not None:
        print(f"wrote {args.json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
