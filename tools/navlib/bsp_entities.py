"""Entity lump reading shared by the offline navigation tools.

Reads the entity lump of a compiled Source BSP through
:func:`navlib.bsp.read_bsp_lumps` and selects the entities that
the navigation checks locate on the mesh: spawns, commanders, barracks,
refineries and resource points.  Standard library only.

Serves ``tools/check_nav.py`` and ``tools/nav_overview.py``.
"""

from __future__ import annotations

from dataclasses import dataclass
from fnmatch import fnmatch
from pathlib import Path
import re

from .bsp import read_bsp_lumps
from .navmesh import Vec3

POINT_CLASSNAME_PATTERNS = (
    "emp_info_player_*",
    "emp_imp_commander",
    "emp_nf_commander",
    "emp_building_*barracks",
    "emp_building_*refinery",
    "emp_resource_point*",
)

_ENTITY_RE = re.compile(r"\{([^{}]*)\}", re.S)
_KEYVALUE_RE = re.compile(r'"([^"]*)"\s+"([^"]*)"')


@dataclass(frozen=True)
class PointEntity:
    name: str  # "<classname>#<index>", unique within one BSP
    classname: str
    targetname: str  # empty when the entity has none
    origin: Vec3


def parse_entities(entity_lump: bytes) -> list[dict[str, str]]:
    """Return the key/value dictionaries of every entity in a BSP entity lump."""

    text = entity_lump.decode("latin-1").rstrip("\x00")
    return [dict(_KEYVALUE_RE.findall(block)) for block in _ENTITY_RE.findall(text)]


def read_bsp_entities(bsp_path: str | Path) -> list[dict[str, str]]:
    """Entity dictionaries of a compiled BSP.  Raises ``BSPReadError`` on bad input."""

    return parse_entities(read_bsp_lumps(bsp_path)[1][0])


def entity_origin(entity: dict[str, str]) -> Vec3 | None:
    raw = entity.get("origin")
    if raw is None:
        return None
    parts = raw.split()
    if len(parts) != 3:
        return None
    try:
        return (float(parts[0]), float(parts[1]), float(parts[2]))
    except ValueError:
        return None


def is_point_classname(classname: str) -> bool:
    return any(fnmatch(classname, pattern) for pattern in POINT_CLASSNAME_PATTERNS)


def select_point_entities(entities: list[dict[str, str]]) -> tuple[tuple[PointEntity, ...], tuple[str, ...]]:
    """Point entities with a usable origin, plus messages for the ones skipped.

    Entities are named by their index in the lump, so the names stay stable
    for the same BSP.
    """

    points: list[PointEntity] = []
    skipped: list[str] = []
    for index, entity in enumerate(entities):
        classname = entity.get("classname", "")
        if not is_point_classname(classname):
            continue
        origin = entity_origin(entity)
        if origin is None:
            skipped.append(f"skipped {classname} #{index}: no usable origin")
            continue
        points.append(
            PointEntity(
                name=f"{classname}#{index}",
                classname=classname,
                targetname=entity.get("targetname", ""),
                origin=origin,
            )
        )
    return tuple(points), tuple(skipped)
