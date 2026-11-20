"""Static prop reading (game lump ``sprp``) for the offline navigation tools (standard library only).

Static props (rocks, walls, palms) are solid for world traces in game but unknown to the
navigation mesh, so vehicles can stall on them where the mesh shows open ground.  Each prop
yields its origin, yaw, model name and solid type (0 none, 1 bounding box, 2 bounding box with
own physics, 6 VPhysics model).  Model bounds are not read; the model file is not part of the BSP.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import struct

from .bsp import BSP_IDENT, BSPReadError

LUMP_GAME_LUMP = 35
_SPRP_ID = (ord("s") << 24) | (ord("p") << 16) | (ord("r") << 8) | ord("p")


@dataclass(frozen=True)
class StaticProp:
    x: float
    y: float
    z: float
    yaw: float
    model: str
    solid: int


def read_static_props(path: str | Path) -> tuple[StaticProp, ...]:
    """Return every static prop of an uncompressed version 19/20 BSP."""

    data = Path(path).read_bytes()
    if len(data) < 8 + 64 * 16 or data[:4] != BSP_IDENT:
        raise BSPReadError(f"not a Source BSP: {path}")
    (version,) = struct.unpack_from("<i", data, 4)
    if version not in (19, 20):
        raise BSPReadError(f"unsupported BSP version {version}: {path}")
    offset, length, _lump_version, fourcc = struct.unpack_from("<iiii", data, 8 + LUMP_GAME_LUMP * 16)
    if fourcc != 0 or offset <= 0 or offset + length > len(data):
        raise BSPReadError(f"game lump unreadable: {path}")
    (count,) = struct.unpack_from("<i", data, offset)
    for index in range(count):
        lump_id, flags, _lump_ver, file_offset, file_length = struct.unpack_from("<iHHii", data, offset + 4 + index * 16)
        if lump_id != _SPRP_ID:
            continue
        if flags & 1:
            raise BSPReadError(f"static prop lump is compressed: {path}")
        return _parse_sprp(data[file_offset : file_offset + file_length])
    return ()


def _parse_sprp(lump: bytes) -> tuple[StaticProp, ...]:
    pos = 0
    (dict_entries,) = struct.unpack_from("<i", lump, pos)
    pos += 4
    names = []
    for _ in range(dict_entries):
        names.append(lump[pos : pos + 128].split(b"\x00", 1)[0].decode("latin-1"))
        pos += 128
    (leaf_entries,) = struct.unpack_from("<i", lump, pos)
    pos += 4 + leaf_entries * 2
    (prop_count,) = struct.unpack_from("<i", lump, pos)
    pos += 4
    if prop_count <= 0:
        return ()
    # The record grows with the lump version (56 bytes in v4 up to 76+); its size follows from the rest.
    stride = (len(lump) - pos) // prop_count
    if stride < 56:
        raise BSPReadError(f"static prop records too short ({stride} bytes)")
    props = []
    for index in range(prop_count):
        base = pos + index * stride
        x, y, z, _pitch, yaw, _roll = struct.unpack_from("<6f", lump, base)
        prop_type, _first_leaf, _leaf_count, solid = struct.unpack_from("<HHHB", lump, base + 24)
        model = names[prop_type] if prop_type < len(names) else "?"
        props.append(StaticProp(x, y, z, yaw, model, solid))
    return tuple(props)
