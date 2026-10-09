#!/usr/bin/env python3
"""Generate examples/spitfire/narvik.telic, the land round Narvik and Bardufoss.

Maintainer tool, run by hand; the generated file is committed, so playing the
game needs no network. It uses the two other generators' functions, placing
every point by the azimuthal equidistant projection from 56 N 3.6 W, true in
distance and bearing from there, as they place the land round Trondheim:

  terrain     tools/gen-norway-terrain.py's heights from the AWS Terrain Tiles
              (Tilezen's Terrarium PNGs at zoom 10; Norway terrain data
              (c) Kartverket) at the corners of a 400 m grid over the box
              68.3-69.12 N, 16.9-18.9 E; heights below 0 (the sea) as 0
  coast       tools/gen-spitfire-map.py's land from OpenStreetMap's coastline
              in that box, (c) OpenStreetMap contributors under the ODbL,
              closed along the box's edges, simplified to 40 m, and its shore
              lines; inland Norway and Sweden east and south of the box as
              two bands of land
  landmarks   Bardufoss Air Station, Bjerkvik, Narvik Church, the Ofotfjord,
              the Herjangsfjord, Andenes and the wreck of Z19 Hermann Kunne from
              Wikidata (CC0), Narvik and Ankenes from OpenStreetMap's place
              points, and the wreck of Z2 Georg Thiele from its wreck node
  Narvik      from OpenStreetMap: the streets of the town's centre, the
              piers, quays and breakwaters 60 m long or more in its harbour,
              and the main-line railways in the box

  python3 tools/gen-narvik.py
"""

import importlib.util
import math
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "examples", "spitfire", "narvik.telic")


def load(name):
    spec = importlib.util.spec_from_file_location(name.replace("-", "_"), os.path.join(ROOT, "tools", name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


terrain = load("gen-norway-terrain")
mapping = load("gen-spitfire-map")

NARVIK_BOX = (68.3, 16.9, 69.12, 18.9)
SPACING = 400.0
ZOOM = 10
TOWN_BOX = "68.425,17.39,68.445,17.44"
HARBOUR_BOX = "68.41,17.36,68.45,17.45"
WIKIDATA_LANDMARKS = [("bardufoss", "Bardufoss Air Station"), ("bjerkvik", "Bjerkvik"), ("narvik-church", "Narvik Church"),
                      ("ofotfjord", "Ofotfjord"), ("herjangsfjord", "Herjangsfjord"),
                      ("hermann-kunne", "German destroyer Z19 Hermann Künne"), ("andenes", "Andenes")]
PLACE_LANDMARKS = [("narvik", "Narvik", "town"), ("ankenes", "Ankenes", "locality")]
WRECK_LANDMARKS = [("georg-thiele", "Z2 Georg Thiele")]


def bands(box):
    south, west, north, east = box
    sampled = mapping.sampled
    east_band = (sampled((east, 67.0), (east, 70.0)) + sampled((east, 70.0), (22.0, 70.0))
                 + sampled((22.0, 70.0), (22.0, 67.0)) + sampled((22.0, 67.0), (east, 67.0)))
    south_band = (sampled((west, 67.0), (east, 67.0)) + sampled((east, 67.0), (east, south))
                  + sampled((east, south), (west, south)) + sampled((west, south), (west, 67.0)))
    rings = [[mapping.azimuthal(lon, lat) for lon, lat in band] for band in (east_band, south_band)]
    return [mapping.simplify(ring + ring[:1], 40.0)[:-1] for ring in rings]


def grid():
    south, west, north, east = NARVIK_BOX
    corners = [terrain.azimuthal(lon, lat) for lon in (west, east) for lat in (south, north)]
    x_low = min(x for x, z in corners)
    x_high = max(x for x, z in corners)
    z_low = min(z for x, z in corners)
    z_high = max(z for x, z in corners)
    columns = int((x_high - x_low) / SPACING) + 1
    rows = int((z_high - z_low) / SPACING) + 1
    points = [terrain.inverse_azimuthal(x_low + c * SPACING, z_low + r * SPACING) for r in range(rows) for c in range(columns)]
    return x_low, z_low, columns, rows, terrain.heights(ZOOM, points)


def places():
    query = '[out:json][timeout:60];\nnode["place"]["name"~"^({})$"]({});\nout;'.format(
        "|".join(name for _, name, _ in PLACE_LANDMARKS), HARBOUR_BOX)
    found = {}
    for element in mapping.overpass(query)["elements"]:
        for key, name, kind in PLACE_LANDMARKS:
            if element["tags"].get("name") == name and element["tags"].get("place") == kind:
                found[key] = mapping.azimuthal(element["lon"], element["lat"])
    return found


def wrecks():
    south, west, north, east = NARVIK_BOX
    query = '[out:json][timeout:60];\nnode["historic"="wreck"]["name"~"^({})$"]({},{},{},{});\nout;'.format(
        "|".join(name for _, name in WRECK_LANDMARKS), south, west, north, east)
    found = {}
    for element in mapping.overpass(query)["elements"]:
        for key, name in WRECK_LANDMARKS:
            if element["tags"].get("name") == name:
                found[key] = mapping.azimuthal(element["lon"], element["lat"])
    return found


def town():
    south, west, north, east = NARVIK_BOX
    query = """[out:json][timeout:180];
(
  way["highway"~"^(primary|secondary|tertiary|residential)$"]({town});
  way["man_made"~"^(pier|quay|breakwater)$"]({harbour});
  way["railway"="rail"]["usage"="main"]({south},{west},{north},{east});
);
out tags geom;""".format(town=TOWN_BOX, harbour=HARBOUR_BOX, south=south, west=west, north=north, east=east)
    streets, piers, railways = [], [], []
    for element in mapping.overpass(query)["elements"]:
        points = [mapping.azimuthal(node["lon"], node["lat"]) for node in element.get("geometry", [])]
        length = sum(math.hypot(b[0] - a[0], b[1] - a[1]) for a, b in zip(points, points[1:]))
        tags = element["tags"]
        if "highway" in tags:
            streets.append(points)
        elif "man_made" in tags and length >= 60:
            piers.append(points)
        elif tags.get("railway") == "rail":
            railways.append(mapping.simplify(points, 20.0))
    return streets, piers, railways


def main():
    x_low, z_low, columns, rows, found = grid()
    land, shores = mapping.coast_land(NARVIK_BOX)
    titled = mapping.titled_points([title for _, title in WIKIDATA_LANDMARKS])
    placed = places()
    placed.update(wrecks())
    streets, piers, railways = town()
    polyline_text = mapping.polyline_text
    lines = [
        "\\ Spitfire: the land round Narvik and Bardufoss, generated by tools/gen-narvik.py",
        "\\ from the AWS Terrain Tiles (Norway terrain data (c) Kartverket), OpenStreetMap",
        "\\ ((c) OpenStreetMap contributors, under the ODbL) and Wikidata (CC0); do not edit,",
        "\\ re-run the script. Points by the azimuthal equidistant projection from 56 N 3.6 W.",
        "",
        "\\ the ground's heights in metres over 68.3-69.12 N, 16.9-18.9 E, row by row from the",
        "\\ south, each row west to east and an array of its own, so no literal outgrows the stack,",
        "\\ its corners every {:.0f} m from the south-west corner; the sea 0".format(SPACING),
        "{:.0f} constant narvik-terrain-west".format(x_low),
        "{:.0f} constant narvik-terrain-south".format(z_low),
        "{:.0f} constant narvik-terrain-spacing".format(SPACING),
        "{} constant narvik-terrain-columns".format(columns),
        "{} constant narvik-terrain-rows".format(rows),
        "[",
    ]
    for r in range(rows):
        lines.append("  [ " + " ".join("{:.0f}".format(max(0.0, h)) for h in found[r * columns:(r + 1) * columns]) + " ]")
    lines += ["] flatten narvik-terrain-rows narvik-terrain-columns matrix constant narvik-terrain-heights", "",
              "\\ the land within the box from OpenStreetMap's coastline, closed outlines",
              "["]
    lines += ["  " + polyline_text(ring) for ring in land]
    lines += ["] to narvik-outlines",
              "\\ inland Norway and Sweden east and south of the box, as two bands of land",
              "["]
    lines += ["  " + polyline_text(ring) for ring in bands(NARVIK_BOX)]
    lines += ["] to narvik-bands",
              "\\ the same coast's shore lines for drawing, pieces over 500 m, without the box's edges",
              "["]
    lines += ["  " + polyline_text(line) for line in shores]
    lines += ["] to narvik-shores", "",
              "\\ the landmarks as [ x z height ], height 0 where Wikidata gives none"]
    for key, title in WIKIDATA_LANDMARKS:
        if title in titled:
            lon, lat, height = titled[title]
            x, z = mapping.azimuthal(lon, lat)
            lines.append("landmarks [ {} {} {} ] :{} ! drop".format(mapping.number(x), mapping.number(z), mapping.number(height), key))
    for key, (x, z) in placed.items():
        lines.append("landmarks [ {} {} 0 ] :{} ! drop".format(mapping.number(x), mapping.number(z), key))
    lines += ["",
              "\\ Narvik's streets' centre lines, each a line of [ x z ] points",
              "["]
    lines += ["  " + polyline_text(line) for line in streets]
    lines += ["] to narvik-streets",
              "\\ the harbour's piers, quays and breakwaters 60 m long or more, today's",
              "["]
    lines += ["  " + polyline_text(line) for line in piers]
    lines += ["] to narvik-piers",
              "\\ the railways, simplified to 20 m",
              "["]
    lines += ["  " + polyline_text(line) for line in railways]
    lines += ["] to narvik-railways", ""]
    with open(OUT, "w") as out:
        out.write("\n".join(lines))
    print("wrote", OUT, rows, "rows", columns, "columns", "land", len(land), "shores", len(shores),
          "streets", len(streets), "piers", len(piers), "railways", len(railways), "landmarks", len(titled) + len(placed))


if __name__ == "__main__":
    main()
