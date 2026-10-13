#!/usr/bin/env python3
"""Generate examples/spitfire/norway-coarse.telic, Norway's and Sweden's ground
beyond the surveyed boxes round Trondheim and Narvik.

Maintainer tool, run by hand; the generated file is committed, so playing the
game needs no network. Every point is placed by the azimuthal equidistant
projection from 56 N 3.6 W, as the other generators place theirs:

  heights     tools/gen-norway-terrain.py's heights from the AWS Terrain Tiles
              (Tilezen's Terrarium PNGs at zoom 8; Norway terrain data
              (c) Kartverket) at the corners of two 1,600 m grids, one over
              62.6-65.8 N, 6-14.5 E and one over 66-70.4 N, 12.5-21 E, each
              covering the box's extent in the game's frame; heights below 0
              (the sea) as 0
  land        Natural Earth's 1:10m land (public domain) within the two grids'
              frame, simplified to 150 m
  patches     for each surveyed box, Trondheim's (gen-spitfire-map.py's
              NORWAY_BOX) and Narvik's (gen-narvik.py's NARVIK_BOX), the
              rectangle 1 km inside the box: Natural Earth's land and
              OpenStreetMap's coastline land ((c) OpenStreetMap contributors
              under the ODbL) each clipped to it, so the rings together put
              the finer coast in the coarse one's place there
  shores      Natural Earth's land's outlines outside the patches' rectangles
              and inside the frame, for the briefing's map

  python3 tools/gen-norway-coarse.py
"""

import importlib.util
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "examples", "spitfire", "norway-coarse.telic")


def load(name):
    spec = importlib.util.spec_from_file_location(name.replace("-", "_"), os.path.join(ROOT, "tools", name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


terrain = load("gen-norway-terrain")
mapping = load("gen-spitfire-map")
narvik = load("gen-narvik")

SOUTH_BOX = (62.6, 6.0, 65.8, 14.5)
NORTH_BOX = (66.0, 12.5, 70.4, 21.0)
SPACING = 1600.0
ZOOM = 8
PATCH_MARGIN = 1000.0
SMALLEST = 20000


def grid(box):
    x_low, z_low, x_high, z_high = mapping.ground_frame([box])
    columns = int((x_high - x_low) / SPACING) + 1
    rows = int((z_high - z_low) / SPACING) + 1
    points = [terrain.inverse_azimuthal(x_low + c * SPACING, z_low + r * SPACING) for r in range(rows) for c in range(columns)]
    return x_low, z_low, columns, rows, terrain.heights(ZOOM, points)


def land(frame):
    x_low, z_low, x_high, z_high = frame
    outlines = []
    for feature in mapping.fetch_json(mapping.NATURAL_EARTH.format("ne_10m_land"))["features"]:
        for ring in mapping.rings_of(feature["geometry"]):
            if not any(55.0 < p[1] < 75.0 and -5.0 < p[0] < 35.0 for p in ring):
                continue
            points = [mapping.azimuthal(p[0], p[1]) for p in ring]
            if points[0] == points[-1]:
                points = points[:-1]
            clipped = mapping.clip_ring(points, x_low, z_low, x_high, z_high)
            if len(clipped) < 3:
                continue
            closed = mapping.simplify(clipped + clipped[:1])[:-1]
            if len(closed) >= 3 and mapping.ring_area(closed) > SMALLEST:
                outlines.append(closed)
    return outlines


def inner_rectangle(box):
    south, west, north, east = box
    west_edge = [mapping.azimuthal(lon, lat) for lon, lat in mapping.sampled((west, south), (west, north)) + [(west, north)]]
    east_edge = [mapping.azimuthal(lon, lat) for lon, lat in mapping.sampled((east, south), (east, north)) + [(east, north)]]
    south_edge = [mapping.azimuthal(lon, lat) for lon, lat in mapping.sampled((west, south), (east, south)) + [(east, south)]]
    north_edge = [mapping.azimuthal(lon, lat) for lon, lat in mapping.sampled((west, north), (east, north)) + [(east, north)]]
    return (max(x for x, z in west_edge) + PATCH_MARGIN, max(z for x, z in south_edge) + PATCH_MARGIN,
            min(x for x, z in east_edge) - PATCH_MARGIN, min(z for x, z in north_edge) - PATCH_MARGIN)


def clipped(rings, rectangle):
    return [ring for ring in (mapping.clip_ring(r, *rectangle) for r in rings) if len(ring) >= 3]


def patch(box, coarse):
    fine, _ = mapping.coast_land(box)
    rectangle = inner_rectangle(box)
    return clipped(coarse, rectangle) + clipped(fine, rectangle), rectangle


def shores(rings, frame, rectangles):
    x_low, z_low, x_high, z_high = frame

    def on_frame(a, b):
        return any(abs(a[i] - edge) < 1 and abs(b[i] - edge) < 1 for i, edge in ((0, x_low), (0, x_high), (1, z_low), (1, z_high)))

    def in_rectangle(p):
        return any(r[0] < p[0] < r[2] and r[1] < p[1] < r[3] for r in rectangles)

    lines = []
    for ring in rings:
        line = []
        for a, b in zip(ring, ring[1:] + ring[:1]):
            middle = ((a[0] + b[0]) / 2, (a[1] + b[1]) / 2)
            if on_frame(a, b) or in_rectangle(middle):
                if len(line) >= 2:
                    lines.append(line)
                line = []
                continue
            if not line:
                line = [a]
            line.append(b)
        if len(line) >= 2:
            lines.append(line)
    return lines


def grid_lines(name, box, cover):
    x_low, z_low, columns, rows, found = grid(box)
    south, west, north, east = box
    lines = ["\\ the ground's heights in metres over {}-{} N, {}-{} E, the {}'s land cover,".format(south, north, west, east, cover),
             "\\ row by row from the south, each row west to east and an array of its own, its",
             "\\ corners every {:.0f} m from the south-west corner; the sea 0".format(SPACING),
             "{:.0f} constant {}-terrain-west".format(x_low, name),
             "{:.0f} constant {}-terrain-south".format(z_low, name),
             "{:.0f} constant {}-terrain-spacing".format(SPACING, name),
             "{} constant {}-terrain-columns".format(columns, name),
             "{} constant {}-terrain-rows".format(rows, name),
             "["]
    for r in range(rows):
        lines.append("  [ " + " ".join("{:.0f}".format(max(0.0, h)) for h in found[r * columns:(r + 1) * columns]) + " ]")
    lines += ["] flatten {0}-terrain-rows {0}-terrain-columns matrix constant {0}-terrain-heights".format(name), ""]
    print(name, rows, "rows", columns, "columns", "highest", max(found), flush=True)
    return lines


def main():
    frame = mapping.ground_frame([SOUTH_BOX, NORTH_BOX])
    coarse = land(frame)
    trondheim, trondheim_rectangle = patch(mapping.NORWAY_BOX, coarse)
    ofoten, narvik_rectangle = patch(narvik.NARVIK_BOX, coarse)
    polyline_text = mapping.polyline_text
    lines = [
        "\\ Spitfire: Norway's and Sweden's ground beyond the surveyed boxes, generated by",
        "\\ tools/gen-norway-coarse.py from the AWS Terrain Tiles (Norway terrain data",
        "\\ (c) Kartverket), Natural Earth's 1:10m land (public domain) and OpenStreetMap",
        "\\ ((c) OpenStreetMap contributors, under the ODbL); do not edit, re-run the script.",
        "\\ Points by the azimuthal equidistant projection from 56 N 3.6 W.",
        "",
    ]
    lines += grid_lines("mid-norway", SOUTH_BOX, "Trøndelag")
    lines += grid_lines("north-norway", NORTH_BOX, "Ofoten")
    lines += ["\\ Norway's and Sweden's land from Natural Earth within the two grids' frame, closed outlines",
              "["]
    lines += ["  " + polyline_text(ring) for ring in coarse]
    lines += ["] to norway-coarse-outlines",
              "\\ round the Trondheimsfjord, within the rectangle 1 km inside the surveyed box, the same land",
              "\\ and OpenStreetMap's coastline land each clipped to it",
              "["]
    lines += ["  " + polyline_text(ring) for ring in trondheim]
    lines += ["] to trondheim-patch",
              "\\ the same round Narvik and Bardufoss",
              "["]
    lines += ["  " + polyline_text(ring) for ring in ofoten]
    lines += ["] to narvik-patch",
              "\\ the Natural Earth land's shore lines outside the two rectangles, for the map",
              "["]
    lines += ["  " + polyline_text(line) for line in shores(coarse, frame, [trondheim_rectangle, narvik_rectangle])]
    lines += ["] to norway-coarse-shores", ""]
    with open(OUT, "w") as out:
        out.write("\n".join(lines))
    print("wrote", OUT, "outlines", len(coarse), "trondheim patch", len(trondheim), "narvik patch", len(ofoten))


if __name__ == "__main__":
    main()
