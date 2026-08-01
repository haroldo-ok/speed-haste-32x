#!/usr/bin/env python3
"""Convert the original SPEEDH.JCL data into the compact 32X runtime format.

The converter follows the structures and coordinate transforms in jclib.c,
racemap.c, sectors.c, is2code.c and object3d.c. It imports both shareware
circuits and the assets required by the 32X edition.
"""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import argparse
from collections import Counter
import math
import shlex
import struct

JCL_MAGIC = 0xDF73B489
IS2_MAGIC = 0x3253492E
I3D_MAGIC = 0x00443342


def u16(v: int) -> bytes:
    return struct.pack("<H", v & 0xFFFF)


def s16(v: int) -> bytes:
    return struct.pack("<h", v)


def u32(v: int) -> bytes:
    return struct.pack("<I", v & 0xFFFFFFFF)


def s32(v: int) -> bytes:
    return struct.pack("<i", v)


class Blob:
    def __init__(self) -> None:
        self.data = bytearray()
        self.offsets: dict[str, int] = {}

    def align(self, amount: int = 4) -> None:
        while len(self.data) % amount:
            self.data.append(0)

    def add(self, name: str, payload: bytes | bytearray, align: int = 4) -> int:
        self.align(align)
        self.offsets[name] = len(self.data)
        self.data.extend(payload)
        return self.offsets[name]


@dataclass
class IS2:
    width: int
    height: int
    dx: int
    dy: int
    xratio: int
    yratio: int
    pixels: bytes


class JCL:
    def __init__(self, filename: Path) -> None:
        data = filename.read_bytes()
        magic, count, _real_count, directory_size = struct.unpack_from("<IIII", data, len(data) - 16)
        if magic != JCL_MAGIC:
            raise ValueError(f"{filename} is not a Speed Haste JCL")
        directory = len(data) - directory_size
        self.files: dict[str, bytes] = {}
        for index in range(count):
            raw_name, backwards_offset, size = struct.unpack_from("<24sii", data, directory + index * 32)
            name = raw_name.split(b"\0", 1)[0].decode("ascii").upper()
            start = directory - backwards_offset
            self.files[name] = data[start:start + size]

    def get(self, name: str) -> bytes:
        return self.files[name.upper()]

    def maybe(self, name: str) -> bytes | None:
        return self.files.get(name.upper())


def decode_is2(data: bytes) -> IS2:
    magic, width, height, dx, dy, xr, yr, flags, length = struct.unpack_from("<IHHhhHHIi", data)
    if magic != IS2_MAGIC:
        raise ValueError("invalid IS2 magic")
    raw = data[24:24 + length]
    lines = height if flags & 1 else width
    offsets: list[int] = []
    cursor = 0
    for _ in range(lines):
        offsets.append(cursor)
        if cursor >= len(raw):
            continue
        cursor += 1
        while cursor < len(raw) and raw[cursor] != 0:
            cursor += raw[cursor] + 1
        cursor += 1

    pixels = bytearray(width * height)  # Zero is transparent to the 32X blitter.
    for line, cursor in enumerate(offsets):
        if cursor >= len(raw):
            continue
        x = raw[cursor]
        cursor += 1
        while cursor < len(raw):
            count = raw[cursor]
            cursor += 1
            for _ in range(count):
                if cursor >= len(raw):
                    break
                color = raw[cursor]
                cursor += 1
                px, py = (x, line) if flags & 1 else (line, x)
                if 0 <= px < width and 0 <= py < height:
                    pixels[py * width + px] = color
                x += 1
            if cursor >= len(raw) or raw[cursor] == 0:
                cursor += 1
                break
            x += raw[cursor]
            cursor += 1
    return IS2(width, height, dx, dy, xr, yr, bytes(pixels))


def rotate_tile(source: bytes, rotation: int) -> bytes:
    if rotation == 0:
        return source
    out = bytearray(4096)
    for y in range(64):
        for x in range(64):
            if rotation == 1:
                sx, sy = y, 63 - x
            elif rotation == 2:
                sx, sy = 63 - x, 63 - y
            else:
                sx, sy = 63 - y, x
            out[y * 64 + x] = source[sy * 64 + sx]
    return bytes(out)


def parse_map(jcl: JCL, number: int):
    """Convert one tDiskMap exactly as racemap.c does.

    Each circuit keeps its own byte-indexed tile atlas. Sharing a combined
    atlas would exceed 256 tile/rotation combinations across the two
    shareware tracks and would make the SH-2 floor inner loop use 16-bit map
    indices, which is measurably slower on 32X.
    """
    data = jcl.get(f"MAP{number:02}.DAT")
    tile_numbers = struct.unpack_from("<4096H", data, 4)
    rotations = data[4 + 8192:4 + 8192 + 4096]
    # Put the most frequently referenced tiles first. The 32X runtime copies
    # this prefix to SDRAM, following D32XR's texture-cache approach while
    # retaining one-byte map indices in the floor inner loop.
    keys = [(tile_number, rotations[index] & 3)
            for index, tile_number in enumerate(tile_numbers)]
    frequency = Counter(keys)
    ordered = sorted(frequency, key=lambda key: (-frequency[key], key[0], key[1]))
    combinations = {key: index for index, key in enumerate(ordered)}
    atlas = bytearray()
    grafs = jcl.get("GRAFS.DAT")
    for tile_number, rotation in ordered:
        source = grafs[tile_number * 4096:(tile_number + 1) * 4096]
        atlas.extend(rotate_tile(source, rotation))
    map64 = bytearray(combinations[key] for key in keys)

    map128 = bytearray(128 * 128)
    for gy in range(32, 96):
        for gx in range(32, 96):
            # tDiskMap is declared Map[64][64], with the X index first.
            map128[gy * 128 + gx] = map64[(gx - 32) * 64 + (gy - 32)]

    things = []
    for offset in range(12292, len(data) - 7, 8):
        x, y, angle, kind = struct.unpack_from("<HHHH", data, offset)
        things.append((x, y, angle, kind))
    cache_coverage = sum(frequency[key] for key in ordered[:28])
    return bytes(map128), bytes(atlas), combinations, things, cache_coverage


def parse_path(jcl: JCL, number: int = 0) -> bytes:
    data = jcl.get(f"MAP{number:02}.PTH")
    version, points, point_size = struct.unpack_from("<iii", data, 192)
    if version != 0 or point_size < 16:
        raise ValueError("unsupported PATH version")
    out = bytearray()
    for index in range(points):
        x, y, direction, speed = struct.unpack_from("<IIii", data, 256 + index * point_size)
        out += u32(x) + u32(y) + s32(direction) + s32(speed)
    return bytes(out)


def trunc_div(numerator: int, denominator: int) -> int:
    """C signed division, needed for sectors.c's exact edge convention."""
    value = abs(numerator) // abs(denominator)
    return -value if (numerator < 0) != (denominator < 0) else value


def point_in_sector(vertices, sides, x: int, y: int) -> bool:
    """Python form of SEC_IsInSector's half-open vertical-ray test."""
    hits = 0
    for v0, v1, _texture, _other in sides:
        x0, y0 = vertices[v0]
        x1, y1 = vertices[v1]
        if not ((x >= x0 and x < x1) or (x < x0 and x >= x1)):
            continue
        if y0 < y and y1 < y:
            hits += 1
        elif y0 < y or y1 < y:
            if x0 < x1:
                iy = y0 + trunc_div((x - x0) * (y1 - y0), x1 - x0)
            else:
                iy = y1 + trunc_div((x - x1) * (y0 - y1), x0 - x1)
            if iy < y:
                hits += 1
    return bool(hits & 1)


def find_source_sector(vertices, sectors, map_x: int, map_y: int) -> int:
    """Assign a DAT thing to the same explicit SEC polygon as the DOS game."""
    x, y = map_x << 4, map_y << 4
    for index, (_flags, sides) in enumerate(sectors):
        if point_in_sector(vertices, sides, x, y):
            return index
    return -1  # Original SEC map's implicit default sector.


def parse_sectors(jcl: JCL, number: int, sprite_id: dict[str, int]):
    """Import the complete SEC topology, not just drawable guardrails.

    Runtime sector coordinates retain the original unsigned 16-bit SEC space.
    Sector records are compact, fixed-size metadata (first side, count, flags,
    bounds), while each side stores vertex indices, adjacency and the optional
    drawable wall index. This lets physics search only the current polygon and
    its neighbours, matching SEC_FindSector without rebuilding pointers.
    """
    lines = [line.strip() for line in jcl.get(f"MAP{number:02}.SEC").decode("ascii").replace("\r", "").split("\n")]
    lines = [line for line in lines if line and not line.startswith(";")]
    vertex_count, sector_count, declared_side_count = (int(v, 0) for v in lines[0].split())
    cursor = 2  # skip header + Vertices
    vertices = []
    for _ in range(vertex_count):
        x, y = (int(v, 0) for v in lines[cursor].split())
        if not (0 <= x <= 0xFFFF and 0 <= y <= 0xFFFF):
            raise ValueError("SEC vertex is outside compact unsigned-16 range")
        vertices.append((x, y))
        cursor += 1
    if lines[cursor].lower() != "sectors":
        raise ValueError("malformed SEC file")
    cursor += 1
    sectors = []
    for _ in range(sector_count):
        side_count, flags = (int(v, 0) for v in lines[cursor].split())
        cursor += 1
        sides = []
        for _ in range(side_count):
            fields = shlex.split(lines[cursor])
            cursor += 1
            v0, v1, other = int(fields[0], 0), int(fields[1], 0), int(fields[3], 0)
            if not (0 <= v0 < vertex_count and 0 <= v1 < vertex_count):
                raise ValueError("SEC side has invalid vertex index")
            if not (-1 <= other < sector_count):
                raise ValueError("SEC side has invalid adjacency")
            sides.append((v0, v1, fields[2], other))
        sectors.append((flags, sides))
    if sum(len(sides) for _flags, sides in sectors) != declared_side_count:
        raise ValueError("SEC side count does not match header")

    walls = []
    sector_records = bytearray()
    side_records = bytearray()
    collision_count = 0
    first_side = 0
    for flags, sides in sectors:
        used_vertices = [vertex for side in sides for vertex in side[:2]]
        xs = [vertices[index][0] for index in used_vertices]
        ys = [vertices[index][1] for index in used_vertices]
        if len(sides) > 255 or flags > 255:
            raise ValueError("SEC sector exceeds compact record")
        sector_records += u16(first_side) + bytes((len(sides), flags))
        sector_records += u16(min(xs)) + u16(min(ys)) + u16(max(xs)) + u16(max(ys))
        first_side += len(sides)

        for v0, v1, texture, other in sides:
            wall_index = 0xFFFF
            if texture:
                other_flags = sectors[other][0] if 0 <= other < len(sectors) else 0
                collision = int(bool(flags) != bool(other_flags))
                collision_count += collision
                x0, y0 = vertices[v0]
                x1, y1 = vertices[v1]
                # SEC_TOMAP: convert once at build time, outside render loops.
                wx0 = ((x0 << 15) + (1 << 30)) & 0xFFFFFFFF
                wy0 = ((y0 << 15) + (1 << 30)) & 0xFFFFFFFF
                wx1 = ((x1 << 15) + (1 << 30)) & 0xFFFFFFFF
                wy1 = ((y1 << 15) + (1 << 30)) & 0xFFFFFFFF
                wall_index = len(walls)
                walls.append((wx0, wy0, wx1, wy1,
                              sprite_id[texture.upper() + ".IS2"], collision))
            side_records += u16(v0) + u16(v1) + s16(other) + u16(wall_index)

    wall_payload = bytearray()
    for x0, y0, x1, y1, texture, collision in walls:
        wall_payload += u32(x0) + u32(y0) + u32(x1) + u32(y1)
        wall_payload += u16(texture) + u16(collision)
    vertex_payload = b"".join(u16(x) + u16(y) for x, y in vertices)
    return (bytes(wall_payload), len(walls), collision_count,
            vertex_payload, bytes(sector_records), bytes(side_records),
            vertex_count, sector_count, declared_side_count,
            vertices, sectors)


def parse_i3d(data: bytes):
    magic, object_size = struct.unpack_from("<II", data)
    if magic != I3D_MAGIC:
        raise ValueError("invalid I3D magic")
    obj = data[8:8 + object_size]
    nverts, _nnormals, nfaces, _nfaceverts, nmaterials, flags = struct.unpack_from("<6H", obj)
    verts_offset, _normals_offset, faces_offset, materials_offset = struct.unpack_from("<4I", obj, 12)
    scx, scy, scz, dcx, dcy, dcz = struct.unpack_from("<6i", obj, 36)
    if scx == 0: scx = 1 << 19
    if scy == 0: scy = 1 << 19
    if scz == 0: scz = 1 << 19
    scx >>= 1; scy >>= 1; scz >>= 1

    vertices = []
    for index in range(nverts):
        x, y, z = struct.unpack_from("<iii", obj, verts_offset + index * 36)
        vertices.append(((x - dcx) * scx >> 16, (y - dcy) * scy >> 16, (z - dcz) * scz >> 16))

    material_colors = []
    for index in range(nmaterials):
        material_colors.append(obj[materials_offset + index * 28])

    faces = []
    face_offset = faces_offset
    seen = set()
    while face_offset and len(faces) < nfaces:
        if face_offset in seen:
            raise ValueError("cyclic I3D face list")
        seen.add(face_offset)
        count = struct.unpack_from("<H", obj, face_offset + 2)[0]
        material_pointer = struct.unpack_from("<I", obj, face_offset + 8)[0]
        material = (material_pointer - materials_offset) // 28 if material_pointer else -1
        color = material_colors[material] if 0 <= material < len(material_colors) else 0
        indices = []
        for index in range(count):
            vertex_pointer = struct.unpack_from("<I", obj, face_offset + 48 + index * 20)[0]
            indices.append((vertex_pointer - verts_offset) // 36)
        if count >= 3 and color:
            faces.append((color, indices))
        face_offset = struct.unpack_from("<I", obj, face_offset + 36)[0]
    return flags, vertices, faces


def rasterize_car_views(model) -> bytes:
    """Pre-render the I3D model through the engine's 16-angle sprite path.

    FS3_Load/FSP_AddObj explicitly support 17 directional bitmap frames. Using
    that path on 32X avoids doing hundreds of divisions per car per frame.
    """
    _flags, vertices, faces = model
    width, height = 64, 48
    atlas = bytearray()

    def fill_poly(image: bytearray, points: list[tuple[int, int]], color: int) -> None:
        miny = max(0, min(y for x, y in points))
        maxy = min(height - 1, max(y for x, y in points))
        for y in range(miny, maxy + 1):
            hits = []
            for i, (x0, y0) in enumerate(points):
                x1, y1 = points[(i + 1) % len(points)]
                if (y0 <= y < y1) or (y1 <= y < y0):
                    hits.append(x0 + (y - y0) * (x1 - x0) // (y1 - y0))
            hits.sort()
            for i in range(0, len(hits) - 1, 2):
                for x in range(max(0, hits[i]), min(width - 1, hits[i + 1]) + 1):
                    image[y * width + x] = color

    for view in range(16):
        a = view * 2.0 * math.pi / 16.0
        ca, sa = math.cos(a), math.sin(a)
        rotated = []
        for x, y, z in vertices:
            rotated.append((x * ca + z * sa, z * ca - x * sa, y))
        min_rx = min(v[0] for v in rotated)
        max_rx = max(v[0] for v in rotated)
        xscale = 42.0 / max(1.0, max_rx - min_rx)
        xcenter = (min_rx + max_rx) * 0.5
        projected = []
        depths = []
        for rx, rz, y in rotated:
            projected.append((round(32 + (rx - xcenter) * xscale),
                              round(43 - y * 0.0055 + rz * 0.00035)))
            depths.append(rz)
        ordered = []
        for color, indices in faces:
            points = [projected[i] for i in indices]
            area = sum(points[i][0] * points[(i + 1) % len(points)][1] -
                       points[(i + 1) % len(points)][0] * points[i][1]
                       for i in range(len(points)))
            if area < 0 or len(points) == 3:
                ordered.append((sum(depths[i] for i in indices) / len(indices), color, points))
        ordered.sort(reverse=True)
        image = bytearray(width * height)
        for _depth, color, points in ordered:
            fill_poly(image, points, color)
        atlas.extend(image)
    return bytes(atlas)


def add_model(blob: Blob, name: str, model) -> tuple[int, int, int]:
    _flags, vertices, faces = model
    payload = bytearray()
    for x, y, z in vertices:
        payload += s32(x) + s32(y) + s32(z)
    face_start = len(payload)
    for color, indices in faces:
        payload += bytes((len(indices), color)) + u16(0)
        for index in indices:
            payload += u16(index)
        if len(payload) & 3:
            payload += b"\0\0"
    offset = blob.add(name, payload)
    return offset, len(vertices), len(faces)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("jcl", type=Path)
    parser.add_argument("--out", type=Path, default=Path("assets/generated"))
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    jcl = JCL(args.jcl)
    blob = Blob()
    blob.add("MAGIC", b"SH32DATA" + u32(2), 4)

    # Trig is stored big-endian so the big-endian SH-2 can index it directly.
    trig = b"".join(struct.pack(">h", round(math.cos(index * 2 * math.pi / 1024) * 32767)) for index in range(1024))
    blob.add("COS_Q15", trig, 4)
    blob.add("GAME_PALETTE", jcl.get("GRAFS.PAL"), 4)
    blob.add("TITLE_PALETTE", jcl.get("SPHLOGO.PAL"), 4)
    blob.add("COLOR_MAP", jcl.get("GRAFS.CLR"), 4)

    track_names = ("Racer's Edge", "The City")
    tracks = []
    for number in range(2):
        map_data, tile_atlas, combinations, things, cache_coverage = parse_map(jcl, number)
        path = parse_path(jcl, number)
        starts = bytearray()
        cameras = bytearray()
        for x, y, angle, kind in things:
            if kind >> 8 == 241:
                starts += u16(x) + u16(y) + u16((0x4000 - angle) & 0xFFFF) + u16(0)
            elif kind >> 8 == 242:
                wx = ((x << 19) + (1 << 30)) & 0xFFFFFFFF
                wy = ((y << 19) + (1 << 30)) & 0xFFFFFFFF
                height = 900 + (kind & 0xFF) * 500
                cameras += u32(wx) + u32(wy) + u32(height)
        tracks.append({
            "number": number, "map": map_data, "tiles": tile_atlas,
            "combinations": combinations, "things": things,
            "path": path, "starts": bytes(starts), "cameras": bytes(cameras),
            "obstacles": [], "cache_coverage": cache_coverage,
        })
        blob.add(f"MAP{number}_MAP128", map_data, 4)
        blob.add(f"MAP{number}_TILES", tile_atlas, 4)
        blob.add(f"MAP{number}_SKY", jcl.get(f"NUBES{number}.PIX"), 4)
        blob.add(f"MAP{number}_MOUNTAINS", jcl.get(f"MOUNT{number}.PIX"), 4)
        blob.add(f"MAP{number}_PATH", path, 4)
        blob.add(f"MAP{number}_STARTS", starts, 4)
        blob.add(f"MAP{number}_CAMERAS", cameras, 4)

    blob.add("COCKPIT0", jcl.get("SALP0.PIX"), 4)
    blob.add("COCKPIT1", jcl.get("SALP1.PIX"), 4)
    blob.add("TITLE", jcl.get("SPHLOGO.PIX"), 4)
    blob.add("MENU_MAIN", jcl.get("MBG_PRIN.PIX"), 4)
    blob.add("MENU_CIRCUIT", jcl.get("MBG_CIRC.PIX"), 4)
    blob.add("MENU_CAR", jcl.get("MBG_CAR.PIX"), 4)

    # Decode wall textures, HUD, countdown, and visible sprites from both maps.
    wall_names = set()
    for number in range(2):
        sec_text = jcl.get(f"MAP{number:02}.SEC").decode("ascii")
        for line in sec_text.replace("\r", "").split("\n"):
            try:
                fields = shlex.split(line.strip())
            except ValueError:
                continue
            if len(fields) == 4 and fields[2]:
                wall_names.add(fields[2].upper() + ".IS2")

    hud_names = [
        "MGEAR.IS2", "MREVO0.IS2", "MREVO1.IS2", "MLAPS.IS2", "MPOS.IS2",
        "MPOSBAR.IS2", "MLAP.IS2", "MBEST.IS2", "PAUSE.IS2", "RFINLAP.IS2",
        "ENDRACE.IS2", "YOUWIN.IS2", "RACE_0.IS2", "RACE_1.IS2",
        "RACE_2.IS2", "RACE_3.IS2", "MFBGB.IS2", "MFMGB.IS2",
        "MWQUOTE.IS2", "MWDQUOTE.IS2", "MGQUOTE.IS2", "MGDQUOTE.IS2",
    ]
    for prefix in ("MFBG", "MFBW", "MFMG", "MFMW", "MFLW", "MFLG", "MG"):
        for digit in range(10):
            hud_names.append(f"{prefix}{digit}.IS2")
    for frame in range(1, 7):
        hud_names.append(f"SPRK{frame:02}AA.IS2")
        for ground in range(3):
            hud_names.append(f"GND{ground}{frame:02}AA.IS2")

    obstacle_names = set()
    for track in tracks:
        obstacle_info = []
        for x, y, angle, kind in track["things"]:
            if kind >= 0xF000:
                continue
            transformed = ((kind & 0xFF00) >> 4) + (kind & 0xF)
            name = f"XPR{transformed:03X}.IS2"
            if jcl.maybe(name):
                obstacle_names.add(name)
                obstacle_info.append((x, y, (0x4000 - angle) & 0xFFFF, name))
        track["obstacles"] = obstacle_info

    sprite_names = sorted(wall_names) + [n for n in hud_names if jcl.maybe(n)] + sorted(obstacle_names)
    sprite_names = list(dict.fromkeys(sprite_names))
    sprite_id = {name: index for index, name in enumerate(sprite_names)}
    sprite_meta = bytearray()
    for name in sprite_names:
        sprite = decode_is2(jcl.get(name))
        pixel_offset = blob.add("SPRITE_" + name.replace(".", "_"), sprite.pixels, 4)
        world_w = sprite.xratio * sprite.width * 0x2000 // (55 << 8)
        world_h = sprite.yratio * sprite.height * 0x2000 // (66 << 8)
        sprite_meta += u32(pixel_offset) + u16(sprite.width) + u16(sprite.height)
        sprite_meta += s16(sprite.dx) + s16(sprite.dy) + u32(world_w) + u32(world_h)
    blob.add("SPRITE_META", sprite_meta, 4)

    for track in tracks:
        number = track["number"]
        (walls, wall_count, collision_count, vertices, sectors, sector_sides,
         vertex_count, sector_count, sector_side_count,
         source_vertices, source_sectors) = parse_sectors(jcl, number, sprite_id)
        track["wall_count"] = wall_count
        track["collision_count"] = collision_count
        track["vertex_count"] = vertex_count
        track["sector_count"] = sector_count
        track["sector_side_count"] = sector_side_count
        blob.add(f"MAP{number}_WALLS", walls, 4)
        blob.add(f"MAP{number}_SECTOR_VERTICES", vertices, 4)
        blob.add(f"MAP{number}_SECTORS", sectors, 4)
        blob.add(f"MAP{number}_SECTOR_SIDES", sector_sides, 4)
        obstacles = bytearray()
        object_buckets: list[list[int]] = [[] for _ in range(sector_count + 1)]
        default_bins: list[list[int]] = [[] for _ in range(16 * 16)]
        for obstacle_index, (x, y, angle, name) in enumerate(track["obstacles"]):
            world_x = ((x << 19) + (1 << 30)) & 0xFFFFFFFF
            world_y = ((y << 19) + (1 << 30)) & 0xFFFFFFFF
            obstacles += u32(world_x) + u32(world_y) + u16(angle) + u16(sprite_id[name])
            sector = find_source_sector(source_vertices, source_sectors, x, y)
            object_buckets[sector if sector >= 0 else sector_count].append(obstacle_index)
            if sector < 0:
                bin_index = ((world_y >> 28) << 4) | (world_x >> 28)
                default_bins[bin_index].append(obstacle_index)
        blob.add(f"MAP{number}_OBSTACLES", obstacles, 4)

        # Per-sector ranges avoid scanning 263/325 objects every frame. The
        # final bucket is the implicit default sector, which SEC_Render always
        # appends after its bounded adjacency traversal.
        sector_object_meta = bytearray()
        sector_object_indices = bytearray()
        first_object = 0
        for bucket in object_buckets:
            sector_object_meta += u16(first_object) + u16(len(bucket))
            sector_object_indices += b"".join(u16(index) for index in bucket)
            first_object += len(bucket)
        if first_object != len(track["obstacles"]):
            raise ValueError("sector object lists lost an obstacle")
        track["default_object_count"] = len(object_buckets[-1])
        blob.add(f"MAP{number}_SECTOR_OBJECT_META", sector_object_meta, 4)
        blob.add(f"MAP{number}_SECTOR_OBJECT_INDICES", sector_object_indices, 4)

        default_bin_meta = bytearray()
        default_bin_indices = bytearray()
        first_default = 0
        for bucket in default_bins:
            default_bin_meta += u16(first_default) + u16(len(bucket))
            default_bin_indices += b"".join(u16(index) for index in bucket)
            first_default += len(bucket)
        if first_default != len(object_buckets[-1]):
            raise ValueError("default-sector spatial bins lost an obstacle")
        track["default_nonempty_bins"] = sum(bool(bucket) for bucket in default_bins)
        blob.add(f"MAP{number}_DEFAULT_OBJECT_BIN_META", default_bin_meta, 4)
        blob.add(f"MAP{number}_DEFAULT_OBJECT_BIN_INDICES", default_bin_indices, 4)

    # Use the high-detail A meshes for the runtime directional frames so front
    # and rear (nose, cockpit, rear wing/deck) remain visually distinct. Keep
    # the compact B meshes as optional polygon/fallback data.
    sprite_models = [[parse_i3d(jcl.get(f"CAR{car_type}N{car}A.I3D"))
                      for car in range(6)] for car_type in range(2)]
    fallback_models = [[parse_i3d(jcl.get(f"CAR{car_type}N{car}B.I3D"))
                        for car in range(6)] for car_type in range(2)]
    car_sprites = bytearray()
    for car_type in range(2):
        for model in sprite_models[car_type]:
            car_sprites += rasterize_car_views(model)
    blob.add("CAR_SPRITES", car_sprites, 4)

    model_meta = bytearray()
    for car_type in range(2):
        for car, model in enumerate(fallback_models[car_type]):
            offset, nvertices, nfaces = add_model(blob, f"CAR{car_type}_{car}", model)
            model_meta += u32(offset) + u16(nvertices) + u16(nfaces)
    blob.add("MODEL_META", model_meta, 4)

    output = args.out / "speed_haste_assets.bin"
    output.write_bytes(blob.data)

    header = [
        "/* Generated by tools/import_speed_haste.py; do not edit. */",
        "#ifndef SPEED_HASTE_ASSETS_GENERATED_H",
        "#define SPEED_HASTE_ASSETS_GENERATED_H",
        "#include <stdint.h>",
        "extern const uint8_t binary_assets_generated_speed_haste_assets_bin_start[];",
        "#define SH_ASSET_BASE (binary_assets_generated_speed_haste_assets_bin_start)",
    ]
    for name, offset in blob.offsets.items():
        header.append(f"#define SHA_{name}_OFF {offset}u")
    header += [
        "#define SHA_TRACK_COUNT 2u",
        "#define SHA_TILE_CACHE_COUNT 28u",
        f"#define SHA_MAP0_SKY_SIZE {len(jcl.get('NUBES0.PIX'))}u",
        f"#define SHA_MAP0_MOUNTAINS_SIZE {len(jcl.get('MOUNT0.PIX'))}u",
        f"#define SHA_MAP1_SKY_SIZE {len(jcl.get('NUBES1.PIX'))}u",
        f"#define SHA_MAP1_MOUNTAINS_SIZE {len(jcl.get('MOUNT1.PIX'))}u",
        f"#define SHA_MAP0_TILE_COUNT {len(tracks[0]['combinations'])}u",
        f"#define SHA_MAP0_PATH_COUNT {len(tracks[0]['path']) // 16}u",
        f"#define SHA_MAP0_START_COUNT {len(tracks[0]['starts']) // 8}u",
        f"#define SHA_MAP0_CAMERA_COUNT {len(tracks[0]['cameras']) // 12}u",
        f"#define SHA_MAP0_WALL_COUNT {tracks[0]['wall_count']}u",
        f"#define SHA_MAP0_COLLISION_WALL_COUNT {tracks[0]['collision_count']}u",
        f"#define SHA_MAP0_SECTOR_VERTEX_COUNT {tracks[0]['vertex_count']}u",
        f"#define SHA_MAP0_SECTOR_COUNT {tracks[0]['sector_count']}u",
        f"#define SHA_MAP0_SECTOR_SIDE_COUNT {tracks[0]['sector_side_count']}u",
        f"#define SHA_MAP0_SECTOR_OBJECT_BUCKET_COUNT {tracks[0]['sector_count'] + 1}u",
        f"#define SHA_MAP0_DEFAULT_OBJECT_COUNT {tracks[0]['default_object_count']}u",
        "#define SHA_MAP0_DEFAULT_OBJECT_BIN_COUNT 256u",
        f"#define SHA_MAP0_DEFAULT_OBJECT_NONEMPTY_BINS {tracks[0]['default_nonempty_bins']}u",
        f"#define SHA_MAP0_OBSTACLE_COUNT {len(tracks[0]['obstacles'])}u",
        f"#define SHA_MAP1_TILE_COUNT {len(tracks[1]['combinations'])}u",
        f"#define SHA_MAP1_PATH_COUNT {len(tracks[1]['path']) // 16}u",
        f"#define SHA_MAP1_START_COUNT {len(tracks[1]['starts']) // 8}u",
        f"#define SHA_MAP1_CAMERA_COUNT {len(tracks[1]['cameras']) // 12}u",
        f"#define SHA_MAP1_WALL_COUNT {tracks[1]['wall_count']}u",
        f"#define SHA_MAP1_COLLISION_WALL_COUNT {tracks[1]['collision_count']}u",
        f"#define SHA_MAP1_SECTOR_VERTEX_COUNT {tracks[1]['vertex_count']}u",
        f"#define SHA_MAP1_SECTOR_COUNT {tracks[1]['sector_count']}u",
        f"#define SHA_MAP1_SECTOR_SIDE_COUNT {tracks[1]['sector_side_count']}u",
        f"#define SHA_MAP1_SECTOR_OBJECT_BUCKET_COUNT {tracks[1]['sector_count'] + 1}u",
        f"#define SHA_MAP1_DEFAULT_OBJECT_COUNT {tracks[1]['default_object_count']}u",
        "#define SHA_MAP1_DEFAULT_OBJECT_BIN_COUNT 256u",
        f"#define SHA_MAP1_DEFAULT_OBJECT_NONEMPTY_BINS {tracks[1]['default_nonempty_bins']}u",
        f"#define SHA_MAP1_OBSTACLE_COUNT {len(tracks[1]['obstacles'])}u",
        f"#define SHA_SPRITE_COUNT {len(sprite_names)}u",
        "enum SHSpriteId {",
    ]
    for name, index in sprite_id.items():
        enum_name = name.replace(".", "_").replace("-", "_")
        header.append(f"    SHSPR_{enum_name} = {index},")
    header += ["};", "#endif", ""]
    (args.out / "speed_haste_assets.h").write_text("\n".join(header))
    (args.out / "manifest.txt").write_text(
        "Tracks: 2 shareware circuits\n"
        f"MAP00 {track_names[0]}: tiles={len(tracks[0]['combinations'])}, "
        f"path={len(tracks[0]['path'])//16}, starts={len(tracks[0]['starts'])//8}, "
        f"cameras={len(tracks[0]['cameras'])//12}, walls={tracks[0]['wall_count']}, "
        f"collision-walls={tracks[0]['collision_count']}, sectors={tracks[0]['sector_count']}, "
        f"sector-sides={tracks[0]['sector_side_count']}, objects={len(tracks[0]['obstacles'])}, "
        f"default-objects={tracks[0]['default_object_count']}, "
        f"default-bins={tracks[0]['default_nonempty_bins']}/256, "
        f"cached-map-coverage={tracks[0]['cache_coverage'] * 100 // 4096}%\n"
        f"MAP01 {track_names[1]}: tiles={len(tracks[1]['combinations'])}, "
        f"path={len(tracks[1]['path'])//16}, starts={len(tracks[1]['starts'])//8}, "
        f"cameras={len(tracks[1]['cameras'])//12}, walls={tracks[1]['wall_count']}, "
        f"collision-walls={tracks[1]['collision_count']}, sectors={tracks[1]['sector_count']}, "
        f"sector-sides={tracks[1]['sector_side_count']}, objects={len(tracks[1]['obstacles'])}, "
        f"default-objects={tracks[1]['default_object_count']}, "
        f"default-bins={tracks[1]['default_nonempty_bins']}/256, "
        f"cached-map-coverage={tracks[1]['cache_coverage'] * 100 // 4096}%\n"
        f"Sprites: {len(sprite_names)}\nCar classes: 2 x 6 I3D models x 16 views\n"
        f"Blob bytes: {len(blob.data)}\n"
    )
    print((args.out / "manifest.txt").read_text(), end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
