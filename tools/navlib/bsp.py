"""Minimal Source BSP lump reader for the offline navigation tools (standard library only).

Only uncompressed BSP version 19/20 files (Source 2007/2009, as used by Empires); version 21
(Left 4 Dead 2) has a different lump header layout and is rejected.
"""

from __future__ import annotations

from pathlib import Path
import struct

BSP_IDENT = b"VBSP"
_HEADER_LUMPS = 64
LUMP_ENTITIES = 0


class BSPReadError(ValueError):
    """Raised for files that are not a readable Source BSP."""


def read_bsp_lumps(path: str | Path, wanted: tuple[int, ...] = (LUMP_ENTITIES,)) -> tuple[int, dict[int, bytes]]:
    """Return (version, {lump index: raw bytes}) for the wanted lumps.  Compressed lumps are rejected."""

    data = Path(path).read_bytes()
    if len(data) < 8 + _HEADER_LUMPS * 16 + 4 or data[:4] != BSP_IDENT:
        raise BSPReadError(f"not a Source BSP: {path}")
    (version,) = struct.unpack_from("<i", data, 4)
    if version not in (19, 20):
        raise BSPReadError(f"unsupported BSP version {version}: {path}")
    lumps: dict[int, bytes] = {}
    for index in wanted:
        offset, length, _lump_version, fourcc = struct.unpack_from("<iiii", data, 8 + index * 16)
        if offset < 0 or length < 0 or offset + length > len(data):
            raise BSPReadError(f"lump {index} lies outside the file: {path}")
        if fourcc != 0:
            raise BSPReadError(f"lump {index} is compressed: {path}")
        lumps[index] = data[offset : offset + length]
    return version, lumps
