#!/usr/bin/env python3
"""Generate examples/spitfire/norway-terrain.telic, the ground's height round
the Trondheimsfjord.

Maintainer tool, run by hand; the generated file is committed, so playing the
game needs no network. It reads the AWS Terrain Tiles (Tilezen's Terrarium
PNGs at zoom 10, about 68 m a pixel here; Norway terrain data (c) Kartverket),
decoding each pixel's height as (red * 256 + green + blue / 256) - 32768
metres, between the four nearest pixels, at the corners of a grid laid in
the game's own frame: metres east (x) and north (z) of 56 N 3.6 W, where
points north of 62 N are placed by the azimuthal equidistant projection from
that point, as tools/gen-spitfire-map.py places the Norwegian coast. The grid
covers the box 63.25-63.80 N, 9.6-11.2 E (that script's NORWAY_BOX) at
400 m, and again at 50 m from the tiles at zoom 13 over 6 km round
Fættenfjord (centred on geography.telic's :faettenfjord, [ 715337 918549 ]),
where the Tirpitz lies 33 m off a cliff; each corner's latitude and longitude
come from the projection's inverse. Heights below 0 (the sea) are written as 0. It also asks
OpenStreetMap's Overpass API, (c) OpenStreetMap contributors under the ODbL,
for Trondheim's harbour: today's piers, quays and breakwaters 60 m long or
more, and the outline of the U-boat bunker Dora I.

  python3 tools/gen-norway-terrain.py
"""

import json
import math
import os
import struct
import time
import urllib.parse
import urllib.request
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "examples", "spitfire", "norway-terrain.telic")
TILES = "https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png"
ZOOM = 10

LAT0 = 56.0
LON0 = -3.6
EARTH_RADIUS = 6371000.0
NORWAY_BOX = (63.25, 9.6, 63.80, 11.2)
SPACING = 400.0
FINE_CENTRE = (715337.0, 918549.0)
FINE_REACH = 3000.0
FINE_SPACING = 50.0
FINE_ZOOM = 13


def azimuthal(lon, lat):
    phi0, lam0 = math.radians(LAT0), math.radians(LON0)
    phi, lam = math.radians(lat), math.radians(lon)
    cos_c = math.sin(phi0) * math.sin(phi) + math.cos(phi0) * math.cos(phi) * math.cos(lam - lam0)
    c = math.acos(max(-1.0, min(1.0, cos_c)))
    k = c / math.sin(c) if c > 0 else 1.0
    x = k * math.cos(phi) * math.sin(lam - lam0)
    z = k * (math.cos(phi0) * math.sin(phi) - math.sin(phi0) * math.cos(phi) * math.cos(lam - lam0))
    return (EARTH_RADIUS * x, EARTH_RADIUS * z)


def inverse_azimuthal(x, z):
    phi0, lam0 = math.radians(LAT0), math.radians(LON0)
    rho = math.hypot(x, z)
    c = rho / EARTH_RADIUS
    lat = math.asin(math.cos(c) * math.sin(phi0) + z * math.sin(c) * math.cos(phi0) / rho)
    lon = lam0 + math.atan2(x * math.sin(c), rho * math.cos(phi0) * math.cos(c) - z * math.sin(phi0) * math.sin(c))
    return (math.degrees(lon), math.degrees(lat))


def png_pixels(data):
    width, height, depth, colour = struct.unpack(">IIBB", data[16:26])
    if depth != 8 or colour not in (2, 6):
        raise SystemExit("unexpected PNG: depth {} colour type {}".format(depth, colour))
    channels = 3 if colour == 2 else 4
    stream, at = b"", 8
    while at < len(data):
        length, kind = struct.unpack(">I4s", data[at:at + 8])
        if kind == b"IDAT":
            stream += data[at + 8:at + 8 + length]
        at += 12 + length
    raw = zlib.decompress(stream)
    stride = width * channels
    rows, previous = [], bytearray(stride)
    for r in range(height):
        kind = raw[r * (stride + 1)]
        line = bytearray(raw[r * (stride + 1) + 1:(r + 1) * (stride + 1)])
        for i in range(stride):
            left = line[i - channels] if i >= channels else 0
            up = previous[i]
            up_left = previous[i - channels] if i >= channels else 0
            if kind == 1:
                line[i] = (line[i] + left) & 255
            elif kind == 2:
                line[i] = (line[i] + up) & 255
            elif kind == 3:
                line[i] = (line[i] + (left + up) // 2) & 255
            elif kind == 4:
                guess = left + up - up_left
                distances = (abs(guess - left), abs(guess - up), abs(guess - up_left))
                nearest = (left, up, up_left)[distances.index(min(distances))]
                line[i] = (line[i] + nearest) & 255
        rows.append(line)
        previous = line
    return width, channels, rows


def tile_heights(zoom, x, y):
    with urllib.request.urlopen(TILES.format(z=zoom, x=x, y=y), timeout=60) as response:
        width, channels, rows = png_pixels(response.read())
    return [[rows[r][c * channels] * 256 + rows[r][c * channels + 1] + rows[r][c * channels + 2] / 256 - 32768
             for c in range(width)] for r in range(len(rows))]


def mercator_pixel(zoom, lon, lat):
    scale = 256 * 2 ** zoom
    px = (lon + 180) / 360 * scale
    py = (1 - math.log(math.tan(math.radians(lat)) + 1 / math.cos(math.radians(lat))) / math.pi) / 2 * scale
    return px, py


def heights(zoom, points):
    tiles = {}

    def at(px, py):
        key = (int(px) // 256, int(py) // 256)
        if key not in tiles:
            tiles[key] = tile_heights(zoom, *key)
        return tiles[key][int(py) % 256][int(px) % 256]

    found = []
    for lon, lat in points:
        px, py = mercator_pixel(zoom, lon, lat)
        x0, y0 = math.floor(px - 0.5), math.floor(py - 0.5)
        fx, fy = px - 0.5 - x0, py - 0.5 - y0
        found.append(at(x0, y0) * (1 - fx) * (1 - fy) + at(x0 + 1, y0) * fx * (1 - fy)
                     + at(x0, y0 + 1) * (1 - fx) * fy + at(x0 + 1, y0 + 1) * fx * fy)
    print("tiles", len(tiles), flush=True)
    return found


OVERPASS = "https://overpass-api.de/api/interpreter"
HARBOUR_BOX = "63.425,10.37,63.45,10.45"


def harbour():
    query = """[out:json][timeout:90];
(
  way["man_made"~"^(pier|quay|breakwater)$"]({box});
  way["name"="Dora 1"]({box});
);
out tags geom;""".format(box=HARBOUR_BOX)
    data = urllib.parse.urlencode({"data": query}).encode()
    request = urllib.request.Request(OVERPASS, data=data,
                                     headers={"User-Agent": "telic-gen-norway-terrain/1", "Accept": "application/json"})
    for attempt in range(5):
        try:
            with urllib.request.urlopen(request, timeout=180) as response:
                elements = json.load(response)["elements"]
            break
        except Exception as error:
            print("overpass: {}; retrying".format(error), flush=True)
            time.sleep(20 * (attempt + 1))
    else:
        raise SystemExit("overpass failed")
    piers, dora = [], None
    for element in elements:
        points = [azimuthal(p["lon"], p["lat"]) for p in element.get("geometry", [])]
        length = sum(math.hypot(b[0] - a[0], b[1] - a[1]) for a, b in zip(points, points[1:]))
        if element["tags"].get("name") == "Dora 1":
            dora = points
        elif length >= 60:
            piers.append(points)
    return piers, dora


def polyline_text(points):
    return "[ " + " ".join("[ {:.0f} {:.0f} ]".format(x, z) for x, z in points) + " ]"


def main():
    piers, dora = harbour()
    south, west, north, east = NORWAY_BOX
    corners = [azimuthal(lon, lat) for lon in (west, east) for lat in (south, north)]
    x_low = min(x for x, z in corners)
    x_high = max(x for x, z in corners)
    z_low = min(z for x, z in corners)
    z_high = max(z for x, z in corners)
    columns = int((x_high - x_low) / SPACING) + 1
    rows = int((z_high - z_low) / SPACING) + 1
    points = [inverse_azimuthal(x_low + c * SPACING, z_low + r * SPACING) for r in range(rows) for c in range(columns)]
    found = heights(ZOOM, points)
    fine_west = FINE_CENTRE[0] - FINE_REACH
    fine_south = FINE_CENTRE[1] - FINE_REACH
    fine_count = int(2 * FINE_REACH / FINE_SPACING) + 1
    fine_points = [inverse_azimuthal(fine_west + c * FINE_SPACING, fine_south + r * FINE_SPACING)
                   for r in range(fine_count) for c in range(fine_count)]
    fine = heights(FINE_ZOOM, fine_points)
    lines = [
        "\\ Spitfire: the ground's height round the Trondheimsfjord, generated by",
        "\\ tools/gen-norway-terrain.py from the AWS Terrain Tiles (Norway terrain",
        "\\ data (c) Kartverket); do not edit, re-run the script. A grid of",
        "\\ heights in metres, row by row from",
        "\\ the south, each row west to east, its corners every {:.0f} m in the".format(SPACING),
        "\\ game's frame from the south-west corner; the sea 0.",
        "",
        "{:.0f} constant terrain-west".format(x_low),
        "{:.0f} constant terrain-south".format(z_low),
        "{:.0f} constant terrain-spacing".format(SPACING),
        "{} constant terrain-columns".format(columns),
        "{} constant terrain-rows".format(rows),
        "[",
    ]
    for r in range(rows):
        row = found[r * columns:(r + 1) * columns]
        lines.append("  " + " ".join("{:.0f}".format(max(0.0, h)) for h in row))
    lines += ["] terrain-rows terrain-columns matrix constant terrain-heights", "",
              "\\ the same at {:.0f} m round Fættenfjord, {:.0f} m each way from [ {:.0f} {:.0f} ],".format(
                  FINE_SPACING, FINE_REACH, FINE_CENTRE[0], FINE_CENTRE[1]),
              "\\ from the tiles at zoom {}".format(FINE_ZOOM),
              "{:.0f} constant fine-terrain-west".format(fine_west),
              "{:.0f} constant fine-terrain-south".format(fine_south),
              "{:.0f} constant fine-terrain-spacing".format(FINE_SPACING),
              "{} constant fine-terrain-count".format(fine_count),
              "["]
    for r in range(fine_count):
        row = fine[r * fine_count:(r + 1) * fine_count]
        lines.append("  " + " ".join("{:.0f}".format(max(0.0, h)) for h in row))
    lines += ["] fine-terrain-count fine-terrain-count matrix constant fine-terrain-heights", "",
              "\\ Trondheim's harbour from OpenStreetMap, (c) OpenStreetMap contributors, under the",
              "\\ ODbL: today's piers, quays and breakwaters 60 m long or more, each a line of",
              "\\ [ x z ] points, and the outline of the U-boat bunker Dora I",
              "["]
    lines += ["  " + polyline_text(pier) for pier in piers]
    lines += ["] constant harbour-piers",
              polyline_text(dora or []) + " constant dora-outline", ""]
    with open(OUT, "w") as out:
        out.write("\n".join(lines))
    print("wrote", OUT, rows, "rows", columns, "columns", "highest", max(found))


if __name__ == "__main__":
    main()
