#!/usr/bin/env python3
"""Generate examples/spitfire/geography.telic, the map the Spitfire flies over.

Maintainer tool, run by hand; the generated file is committed, so playing the
game needs no network. It downloads Natural Earth's 1:10m vectors (public
domain) and asks Wikidata (CC0) for the coordinates and heights of the
landmarks, then writes them in the game's ground frame: metres east (x) and
north (z) of 56°N 3.6°W, from an equirectangular projection about that point,
54.9-58.7°N, 5.2-1.3°W. Its east-west distances from 3.6°W are true at 56°N
and off by the cosine of the latitude elsewhere: 3.4 km long at Tynemouth,
4 km at the map's south-east corner, 10 km short at its north-east corner.

  python3 tools/gen-spitfire-map.py

The file holds
  coastlines   arrays of [ x z ] points, sea level, simplified to 150 m
  lochs        Loch Lomond and Loch Ness, closed, with their surface heights
  rivers       the Ness from Natural Earth; the Clyde through Glasgow, the
               Forth up to Stirling and the Kelvin from the Botanic Gardens
               through Kelvingrove Park to the Clyde, which Natural Earth
               lacks, traced through Wikidata places on their banks (one
               Kelvin point the midpoint of the museum and the Kelvin Hall,
               between which it flows)
  land squares the south-west corner and size of the kilometre squares the
               countryside is laid out in
  land outlines
               Natural Earth's land polygons clipped to the map's edges and
               simplified to 150 m, closed rings, for filling the water
  map edges    the frame the outlines are clipped to; south land, England as
               a band of land south of the frame, so the sea outside the
               map's land is water and England is not
  Norway       the land round the Trondheimsfjord from OpenStreetMap's
               coastline (land on its left), chained, clipped to the box and
               closed along it, simplified to 40 m, islets under 2 hectares
               dropped. Points north of 62 N are placed by the azimuthal
               equidistant projection from 56 N 3.6 W, true in distance and
               bearing from there, so the flight from Wick to Trondheim is its
               true length
  cities       Glasgow and Edinburgh from OpenStreetMap through its Overpass
               API, (c) OpenStreetMap contributors under the ODbL, which
               therefore covers the generated file: each city's main streets'
               centre lines (a street's ways averaged across every 80 m along
               it; the Royal Mile is Castlehill, Lawnmarket, High Street and
               Canongate together), its squares' outlines (the largest closed
               way by the name that is neither a road nor a railway, else the
               convex hull of the streets bearing the square's name), its
               stations' outlines (the largest closed station or station
               building by each name, else the largest unnamed station
               building in the city), named buildings' outlines for the
               landmarks' footprints, and its main-line railways,
               a way left out when most of it lies within 20 m of a longer
               line already kept
  landmarks    a frame of name to [ x z height ], height 0 where Wikidata has
               none
"""

import json
import math
import os
import time
import urllib.error
import urllib.parse
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "examples", "spitfire", "geography.telic")
NATURAL_EARTH = "https://raw.githubusercontent.com/nvkelso/natural-earth-vector/master/geojson/{}.geojson"
WIKIDATA = "https://www.wikidata.org/w/api.php?action=wbgetentities&sites=enwiki&props=claims&format=json&titles="

LAT0 = 56.0
LON0 = -3.6
METRES_PER_DEGREE_NORTH = 110574.0
METRES_PER_DEGREE_EAST = 111320.0 * math.cos(math.radians(LAT0))
LAT_MIN, LAT_MAX, LON_MIN, LON_MAX = 54.9, 58.7, -5.2, -1.3
SIMPLIFY_METRES = 150.0
CELL_METRES = 1000.0

LANDMARKS = [
    ("edinburgh-castle", "Edinburgh Castle"), ("arthurs-seat", "Arthur's Seat"),
    ("calton-hill", "Calton Hill"), ("holyrood", "Holyrood Palace"),
    ("scott-monument", "Scott Monument"), ("leith", "Leith"), ("granton", "Granton, Edinburgh"),
    ("portobello", "Portobello, Edinburgh"), ("forth-bridge", "Forth Bridge"),
    ("inchgarvie", "Inchgarvie"), ("north-queensferry", "North Queensferry"),
    ("inchkeith", "Inchkeith"), ("inchcolm", "Inchcolm"), ("cramond-island", "Cramond Island"),
    ("hound-point", "Hound Point"), ("turnhouse", "Edinburgh Airport"), ("drem", "RAF Drem"),
    ("north-berwick-law", "North Berwick Law"), ("bass-rock", "Bass Rock"),
    ("scald-law", "Scald Law"), ("ben-cleuch", "Ben Cleuch"), ("west-lomond", "West Lomond"),
    ("stirling-castle", "Stirling Castle"), ("wallace-monument", "Wallace Monument"),
    ("glasgow-cathedral", "Glasgow Cathedral"), ("glasgow-city-chambers", "Glasgow City Chambers"),
    ("glasgow-university", "University of Glasgow"), ("kelvingrove", "Kelvingrove Art Gallery and Museum"),
    ("glasgow-green", "Glasgow Green"), ("govan", "Govan"), ("clydebank", "Clydebank"),
    ("abbotsinch", "Glasgow Airport"), ("dumbarton-castle", "Dumbarton Castle"), ("greenock", "Greenock"),
    ("earls-seat", "Earl's Seat"), ("ben-lomond", "Ben Lomond"), ("tinto", "Tinto"),
    ("inverness-castle", "Inverness Castle"), ("inverness-cathedral", "Inverness Cathedral"),
    ("craig-phadrig", "Craig Phadrig"), ("dalcross", "Inverness Airport"),
    ("urquhart-castle", "Urquhart Castle"), ("wick", "Wick Airport"),
    ("nidaros", "Nidaros Cathedral"), ("munkholmen", "Munkholmen"), ("vaernes", "Værnes Air Station"),
    ("faettenfjord", "Fættenfjorden"),
    ("st-margarets-chapel", "St Margaret's Chapel, Edinburgh"), ("war-memorial", "Scottish National War Memorial"),
    ("st-giles", "St Giles' Cathedral"), ("the-hub", "The Hub, Edinburgh"), ("tron-kirk", "Tron Kirk"),
    ("balmoral-hotel", "Balmoral Hotel"), ("waverley", "Edinburgh Waverley railway station"),
    ("north-bridge", "North Bridge, Edinburgh"), ("nelson-monument", "Nelson Monument, Edinburgh"),
    ("national-monument", "National Monument of Scotland"), ("dugald-stewart", "Dugald Stewart Monument"),
    ("st-marys-cathedral", "St Mary's Cathedral, Edinburgh (Episcopal)"), ("fettes-college", "Fettes College"),
    ("tolbooth-steeple", "Glasgow Tolbooth"), ("glasgow-necropolis", "Glasgow Necropolis"),
    ("trinity-college", "Trinity College, Glasgow"),
    ("finnieston-crane", "Finnieston Crane"), ("titan-crane", "Titan Clydebank"),
    ("farne-islands", "Farne Islands"), ("acklington", "RAF Acklington"), ("tynemouth", "Tynemouth"),
    ("bamburgh-castle", "Bamburgh Castle"), ("lindisfarne-castle", "Lindisfarne Castle"),
]

CLYDE = ["Greenock", "Port Glasgow", "Bowling, West Dunbartonshire", "Old Kilpatrick", "Erskine Bridge",
         "Clydebank", "Renfrew", "Govan", "Riverside Museum", "Kingston Bridge, Glasgow", "Glasgow Green",
         "Dalmarnock", "Rutherglen"]
FORTH = ["Grangemouth", "Kincardine Bridge", "Clackmannan", "Alloa", "Cambuskenneth Abbey", "Stirling Old Bridge"]
KELVIN = [("Glasgow Botanic Gardens",), ("Kelvinbridge subway station",), ("Kelvingrove Park",),
          ("Kelvingrove Art Gallery and Museum", "Kelvin Hall"), ("Riverside Museum",)]


def traced(stations, places):
    course = []
    for titles in stations:
        if all(t in places for t in titles):
            points = [to_ground(*places[t][:2]) for t in titles]
            course.append((sum(p[0] for p in points) / len(points), sum(p[1] for p in points) / len(points)))
    return course


EARTH_RADIUS = 6371000.0
NORWAY_BOX = (63.25, 9.6, 63.80, 11.2)
BOUNDARY_STEP = 0.02


def sampled(start, end):
    steps = max(1, int(max(abs(end[0] - start[0]), abs(end[1] - start[1])) / BOUNDARY_STEP))
    return [(start[0] + (end[0] - start[0]) * s / steps, start[1] + (end[1] - start[1]) * s / steps) for s in range(steps)]


def norway_bands(box):
    south, west, north, east = box
    east_band = (sampled((east, 60.0), (east, 66.0)) + sampled((east, 66.0), (16.0, 66.0))
                 + sampled((16.0, 66.0), (16.0, 60.0)) + sampled((16.0, 60.0), (east, 60.0)))
    south_band = (sampled((west, 60.0), (east, 60.0)) + sampled((east, 60.0), (east, south))
                  + sampled((east, south), (west, south)) + sampled((west, south), (west, 60.0)))
    rings = [[azimuthal(lon, lat) for lon, lat in band] for band in (east_band, south_band)]
    return [simplify(ring + ring[:1], 40.0)[:-1] for ring in rings]


def azimuthal(lon, lat):
    phi0, lam0 = math.radians(LAT0), math.radians(LON0)
    phi, lam = math.radians(lat), math.radians(lon)
    cos_c = math.sin(phi0) * math.sin(phi) + math.cos(phi0) * math.cos(phi) * math.cos(lam - lam0)
    c = math.acos(max(-1.0, min(1.0, cos_c)))
    k = c / math.sin(c) if c > 0 else 1.0
    x = k * math.cos(phi) * math.sin(lam - lam0)
    z = k * (math.cos(phi0) * math.sin(phi) - math.sin(phi0) * math.cos(phi) * math.cos(lam - lam0))
    return (EARTH_RADIUS * x, EARTH_RADIUS * z)


def to_ground(lon, lat):
    if lat > 62:
        return azimuthal(lon, lat)
    return ((lon - LON0) * METRES_PER_DEGREE_EAST, (lat - LAT0) * METRES_PER_DEGREE_NORTH)


def inside(lon, lat):
    return LAT_MIN <= lat <= LAT_MAX and LON_MIN <= lon <= LON_MAX


def fetch_json(url):
    request = urllib.request.Request(url, headers={"User-Agent": "telic-gen-spitfire-map/1"})
    with urllib.request.urlopen(request, timeout=120) as response:
        return json.load(response)


def rings_of(geometry):
    kind = geometry["type"]
    if kind == "LineString":
        return [geometry["coordinates"]]
    if kind in ("MultiLineString", "Polygon"):
        return geometry["coordinates"]
    if kind == "MultiPolygon":
        return [ring for polygon in geometry["coordinates"] for ring in polygon]
    return []


def runs_inside(ring):
    runs, run = [], []
    for lon, lat in ((p[0], p[1]) for p in ring):
        if inside(lon, lat):
            run.append(to_ground(lon, lat))
        elif run:
            runs.append(run)
            run = []
    if run:
        runs.append(run)
    return [r for r in runs if len(r) > 1]


def distance_to_segment(p, a, b):
    ax, az = a
    bx, bz = b
    dx, dz = bx - ax, bz - az
    length = dx * dx + dz * dz
    t = 0.0 if length == 0 else max(0.0, min(1.0, ((p[0] - ax) * dx + (p[1] - az) * dz) / length))
    return math.hypot(p[0] - ax - t * dx, p[1] - az - t * dz)


def simplify(points, tolerance=SIMPLIFY_METRES):
    if len(points) < 3:
        return points
    keep = [False] * len(points)
    keep[0] = keep[-1] = True
    stack = [(0, len(points) - 1)]
    while stack:
        first, last = stack.pop()
        farthest, index = 0.0, None
        for k in range(first + 1, last):
            d = distance_to_segment(points[k], points[first], points[last])
            if d > farthest:
                farthest, index = d, k
        if index is not None and farthest > tolerance:
            keep[index] = True
            stack.append((first, index))
            stack.append((index, last))
    return [p for p, k in zip(points, keep) if k]


def layer_runs(name, wanted=None):
    runs = []
    for feature in fetch_json(NATURAL_EARTH.format(name))["features"]:
        if feature["geometry"] is None:
            continue
        if wanted and feature["properties"].get("name") not in wanted:
            continue
        for ring in rings_of(feature["geometry"]):
            runs.extend(simplify(run) for run in runs_inside(ring))
    return runs


def clip_ring(ring, x_low, z_low, x_high, z_high):
    sides = [(lambda p: p[0] >= x_low, lambda a, b: (x_low, a[1] + (x_low - a[0]) * (b[1] - a[1]) / (b[0] - a[0]))),
             (lambda p: p[0] <= x_high, lambda a, b: (x_high, a[1] + (x_high - a[0]) * (b[1] - a[1]) / (b[0] - a[0]))),
             (lambda p: p[1] >= z_low, lambda a, b: (a[0] + (z_low - a[1]) * (b[0] - a[0]) / (b[1] - a[1]), z_low)),
             (lambda p: p[1] <= z_high, lambda a, b: (a[0] + (z_high - a[1]) * (b[0] - a[0]) / (b[1] - a[1]), z_high))]
    for keeps, cut in sides:
        clipped = []
        for a, b in zip(ring, ring[1:] + ring[:1]):
            if keeps(b):
                if not keeps(a):
                    clipped.append(cut(a, b))
                clipped.append(b)
            elif keeps(a):
                clipped.append(cut(a, b))
        ring = clipped
        if not ring:
            return []
    return ring


def ring_area(ring):
    return abs(sum(a[0] * b[1] - b[0] * a[1] for a, b in zip(ring, ring[1:] + ring[:1]))) / 2


def land_outlines():
    x_low, z_low = to_ground(LON_MIN, LAT_MIN)
    x_high, z_high = to_ground(LON_MAX, LAT_MAX)
    outlines = []
    for feature in fetch_json(NATURAL_EARTH.format("ne_10m_land"))["features"]:
        for ring in rings_of(feature["geometry"]):
            points = [to_ground(p[0], p[1]) for p in ring]
            if points[0] == points[-1]:
                points = points[:-1]
            clipped = clip_ring(points, x_low, z_low, x_high, z_high)
            if len(clipped) < 3:
                continue
            closed = simplify(clipped + clipped[:1])[:-1]
            if len(closed) >= 3 and ring_area(closed) > 20000:
                outlines.append(closed)
    return outlines


OVERPASS = "https://overpass-api.de/api/interpreter"
CITIES = {
    "glasgow": {
        "box": "55.835,-4.33,55.885,-4.20",
        "streets": {"great-western-road": ["Great Western Road"], "argyle-street": ["Argyle Street"],
                    "buchanan-street": ["Buchanan Street"], "sauchiehall-street": ["Sauchiehall Street"]},
        "squares": ["George Square"],
        "stations": ["Glasgow Central", "Glasgow Queen Street"],
    },
    "edinburgh": {
        "box": "55.935,-3.25,55.965,-3.16",
        "streets": {"royal-mile": ["Castlehill", "Lawnmarket", "High Street", "Canongate"],
                    "princes-street": ["Princes Street"], "george-street": ["George Street"],
                    "queen-street": ["Queen Street"], "south-bridge": ["South Bridge"],
                    "leith-walk": ["Leith Walk"], "lothian-road": ["Lothian Road"]},
        "squares": ["St Andrew Square", "Charlotte Square"],
        "stations": ["Edinburgh Waverley", "Haymarket"],
    },
    "inverness": {
        "box": "57.468,-4.25,57.492,-4.21",
        "streets": {"academy-street": ["Academy Street"], "high-street": ["High Street"],
                    "church-street": ["Church Street"], "castle-street": ["Castle Street"],
                    "bridge-street": ["Bridge Street"], "union-street": ["Union Street"],
                    "tomnahurich-street": ["Tomnahurich Street"], "huntly-street": ["Huntly Street"]},
        "squares": [],
        "stations": ["Inverness"],
        "buildings": ["Inverness Castle", "Inverness Town House"],
    },
    "trondheim": {
        "box": "63.40,10.35,63.50,10.95",
        "streets": {"munkegata": ["Munkegata"], "kongens-gate": ["Kongens gate"],
                    "olav-tryggvasons-gate": ["Olav Tryggvasons gate"], "prinsens-gate": ["Prinsens gate"]},
        "squares": [],
        "stations": ["Trondheim S"],
        "buildings": [],
    },
}


def city_query(city):
    box = city["box"]
    names = "|".join(n for names in city["streets"].values() for n in names)
    squares = "|".join(city["squares"]) or "^$"
    stations = "|".join(city["stations"])
    buildings = "|".join(city.get("buildings", [])) or "^$"
    return """[out:json][timeout:120];
(
  way["highway"]["name"~"^({names})$"]({box});
  way["name"~"^({squares})$"]({box});
  way["name"~"^({stations})$"]["railway"="station"]({box});
  way["building"="train_station"]({box});
  way["building"]["name"~"^({buildings})$"]({box});
  way["railway"="rail"]["usage"="main"]({box});
);
out geom;""".format(names=names, squares=squares, stations=stations, buildings=buildings, box=box)


def overpass(query, attempts=5):
    data = urllib.parse.urlencode({"data": query}).encode()
    for attempt in range(attempts):
        request = urllib.request.Request(OVERPASS, data=data, headers={"User-Agent": "telic-gen-spitfire-map/1"})
        try:
            with urllib.request.urlopen(request, timeout=300) as response:
                return json.load(response)
        except urllib.error.HTTPError as error:
            if error.code not in (429, 502, 503, 504) or attempt == attempts - 1:
                raise
            time.sleep(30)


def way_points(element):
    return [to_ground(node["lon"], node["lat"]) for node in element["geometry"]]


def centre_line(ways, step=80.0):
    points = [p for way in ways for p in way]
    mean_x = sum(p[0] for p in points) / len(points)
    mean_z = sum(p[1] for p in points) / len(points)
    sxx = sum((p[0] - mean_x) ** 2 for p in points)
    szz = sum((p[1] - mean_z) ** 2 for p in points)
    sxz = sum((p[0] - mean_x) * (p[1] - mean_z) for p in points)
    angle = 0.5 * math.atan2(2 * sxz, sxx - szz)
    along_x, along_z = math.cos(angle), math.sin(angle)
    bins = {}
    for x, z in points:
        along = (x - mean_x) * along_x + (z - mean_z) * along_z
        bins.setdefault(int(along // step), []).append((x, z))
    line = [(sum(p[0] for p in group) / len(group), sum(p[1] for p in group) / len(group))
            for _, group in sorted(bins.items())]
    return simplify(line, 15.0)


def distinct_lines(ways, reach=20.0, shortest=150.0):
    def length(way):
        return sum(math.hypot(b[0] - a[0], b[1] - a[1]) for a, b in zip(way, way[1:]))
    kept = []
    for way in sorted(ways, key=length, reverse=True):
        if length(way) < shortest:
            continue
        segments = [s for line in kept for s in zip(line, line[1:])]
        near = sum(1 for p in way if any(distance_to_segment(p, a, b) < reach for a, b in segments))
        if near < 0.8 * len(way):
            kept.append(simplify(way, 15.0))
    return kept


def ring_area_of(points):
    return ring_area(points[:-1]) if len(points) > 3 and points[0] == points[-1] else 0


def largest_ring(elements, name, wanted):
    rings = [way_points(e) for e in elements if e["tags"].get("name") == name and wanted(e["tags"])]
    rings = [r for r in rings if ring_area_of(r) > 0]
    return simplify(max(rings, key=ring_area_of), 5.0)[:-1]


def convex_hull(points):
    points = sorted(set(points))
    def half(sequence):
        hull = []
        for p in sequence:
            while len(hull) >= 2 and ((hull[-1][0] - hull[-2][0]) * (p[1] - hull[-2][1])
                                      - (hull[-1][1] - hull[-2][1]) * (p[0] - hull[-2][0])) <= 0:
                hull.pop()
            hull.append(p)
        return hull
    lower = half(points)
    upper = half(reversed(points))
    return lower[:-1] + upper[:-1]


def square_outline(elements, name):
    def bounding(tags):
        return not tags.get("highway") and not tags.get("railway")
    if any(e["tags"].get("name") == name and bounding(e["tags"]) and ring_area_of(way_points(e)) > 0 for e in elements):
        return largest_ring(elements, name, bounding)
    streets = [p for e in elements if e["tags"].get("name") == name and e["tags"].get("highway") for p in way_points(e)]
    return simplify(convex_hull(streets) + convex_hull(streets)[:1], 5.0)[:-1]


def chain_ways(ways):
    chains = [list(w) for w in ways]
    merged = True
    while merged:
        merged = False
        starts = {}
        for i, chain in enumerate(chains):
            if chain[0] != chain[-1]:
                starts.setdefault(chain[0], i)
        for i, chain in enumerate(chains):
            if chain is None or chain[0] == chain[-1]:
                continue
            j = starts.get(chain[-1])
            if j is not None and j != i and chains[j] is not None:
                chains[i] = chain + chains[j][1:]
                chains[j] = None
                merged = True
        chains = [c for c in chains if c is not None]
    return chains


def boundary_position(point, box):
    south, west, north, east = box
    lon, lat = point
    width, height = east - west, north - south
    if abs(lat - south) < 1e-9:
        return lon - west
    if abs(lon - east) < 1e-9:
        return width + lat - south
    if abs(lat - north) < 1e-9:
        return width + height + east - lon
    return 2 * width + height + north - lat


def boundary_point(position, box):
    south, west, north, east = box
    width, height = east - west, north - south
    position %= 2 * (width + height)
    if position < width:
        return (west + position, south)
    if position < width + height:
        return (east, south + position - width)
    if position < 2 * width + height:
        return (east - (position - width - height), north)
    return (west, north - (position - 2 * width - height))


def clip_chain(chain, box):
    south, west, north, east = box
    def inside(p):
        return west <= p[0] <= east and south <= p[1] <= north
    def clamped(p):
        return (min(max(p[0], west), east), min(max(p[1], south), north))
    def crossing(a, b):
        return exact_crossing(a, b) or clamped(a if inside(a) else b)
    def exact_crossing(a, b):
        best = None
        for edge, value in (("lon", west), ("lon", east), ("lat", south), ("lat", north)):
            index = 0 if edge == "lon" else 1
            if (a[index] - value) * (b[index] - value) < 0:
                t = (value - a[index]) / (b[index] - a[index])
                p = (a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1]))
                p = (min(max(p[0], west), east), min(max(p[1], south), north))
                if inside(p) and (best is None or t < best[0]):
                    best = (t, p)
        return best[1] if best else None
    pieces, piece = [], []
    for a, b in zip(chain, chain[1:]):
        if inside(a):
            if not piece:
                piece = [a]
            if inside(b):
                piece.append(b)
            else:
                piece.append(crossing(a, b))
                pieces.append(piece)
                piece = []
        elif inside(b):
            piece = [crossing(a, b), b]
    if piece:
        pieces.append(piece)
    return pieces


def coast_land(box):
    south, west, north, east = box
    query = "[out:json][timeout:180];\nway[\"natural\"=\"coastline\"]({},{},{},{});\nout geom;".format(south, west, north, east)
    ways = [[(n["lon"], n["lat"]) for n in e["geometry"]] for e in overpass(query)["elements"]]
    chains = chain_ways(ways)
    rings = [c[:-1] for c in chains if c[0] == c[-1]]
    def on_boundary(p):
        return min(abs(p[1] - south), abs(p[1] - north), abs(p[0] - west), abs(p[0] - east)) < 1e-9
    pieces = [p for c in chains if c[0] != c[-1] for p in clip_chain(c, box)]
    on_edge = [p for p in pieces if len(p) >= 2 and on_boundary(p[0]) and on_boundary(p[-1])]
    width, height = east - west, north - south
    perimeter = 2 * (width + height)
    corner_positions = [width, width + height, 2 * width + height, 0.0]
    unused = list(range(len(on_edge)))
    while unused:
        first = unused.pop(0)
        ring = list(on_edge[first])
        current = first
        while True:
            exit_at = boundary_position(on_edge[current][-1], box)
            candidates = [(((boundary_position(on_edge[i][0], box) - exit_at) % perimeter), i) for i in unused + [first]]
            gap, following = min(candidates)
            steps = int(gap / BOUNDARY_STEP)
            offsets = {gap * s / (steps + 1) for s in range(1, steps + 1)}
            offsets |= {(c - exit_at) % perimeter for c in corner_positions if 0 < (c - exit_at) % perimeter < gap}
            for offset in sorted(offsets):
                ring.append(boundary_point(exit_at + offset, box))
            if following == first:
                break
            unused.remove(following)
            ring.extend(on_edge[following])
            current = following
        rings.append(ring)
    land = []
    for ring in rings:
        points = [to_ground(lon, lat) for lon, lat in ring]
        closed = simplify(points + points[:1], 40.0)[:-1]
        if len(closed) >= 3 and ring_area(closed) > 20000:
            land.append(closed)
    shores = []
    for chain in chains:
        for piece in ([chain] if chain[0] == chain[-1] else clip_chain(chain, box)):
            points = simplify([to_ground(lon, lat) for lon, lat in piece], 40.0)
            if len(points) >= 2 and sum(math.hypot(b[0] - a[0], b[1] - a[1]) for a, b in zip(points, points[1:])) > 500:
                shores.append(points)
    return land, shores


def key_of(name):
    return name.lower().replace(" ", "-")


def city_features(city):
    elements = overpass(city_query(city))["elements"]
    streets = {key: centre_line([way_points(e) for e in elements
                                 if e["tags"].get("highway") and e["tags"].get("name") in names])
               for key, names in city["streets"].items()}
    squares = {key_of(name): square_outline(elements, name) for name in city["squares"]}
    stations = {key_of(name): station_outline(elements, name) for name in city["stations"]}
    buildings = {key_of(name): largest_ring(elements, name, lambda tags: bool(tags.get("building")))
                 for name in city.get("buildings", [])}
    railways = distinct_lines([way_points(e) for e in elements if e["tags"].get("railway") == "rail"])
    return streets, squares, stations, buildings, railways


def station_outline(elements, name):
    def named_station(tags):
        return tags.get("railway") == "station" or tags.get("building") == "train_station"
    if any(e["tags"].get("name") == name and named_station(e["tags"]) and ring_area_of(way_points(e)) > 0 for e in elements):
        return largest_ring(elements, name, named_station)
    unnamed = [way_points(e) for e in elements
               if e["tags"].get("building") == "train_station" and not e["tags"].get("name")]
    return simplify(max(unnamed, key=ring_area_of), 5.0)[:-1]


def titled_points(titles):
    found = {}
    for i in range(0, len(titles), 40):
        chunk = titles[i:i + 40]
        url = WIKIDATA.replace("props=claims", "props=claims|sitelinks&sitefilter=enwiki") + urllib.parse.quote("|".join(chunk))
        for entity in fetch_json(url)["entities"].values():
            title = entity.get("sitelinks", {}).get("enwiki", {}).get("title")
            claims = entity.get("claims", {})
            if title is None or not claims.get("P625"):
                continue
            value = claims["P625"][0]["mainsnak"]["datavalue"]["value"]
            height = 0.0
            if claims.get("P2044"):
                elevation = claims["P2044"][0]["mainsnak"]["datavalue"]["value"]
                height = float(elevation["amount"])
                if elevation.get("unit", "").endswith("/Q3710"):
                    height *= 0.3048
            found[title] = (value["longitude"], value["latitude"], height)
    return found


def number(v):
    return "{:.0f}".format(v)


def polyline_text(points):
    return "[ " + " ".join("[ {} {} ]".format(number(x), number(z)) for x, z in points) + " ]"


def main():
    coast = layer_runs("ne_10m_coastline")
    lomond = layer_runs("ne_10m_lakes_europe", {"Loch Lomond North Basin", None})
    lomond = [run for run in lomond
              if all(-4.8 < x / METRES_PER_DEGREE_EAST + LON0 < -4.4 and 55.95 < z / METRES_PER_DEGREE_NORTH + LAT0 < 56.35
                     for x, z in run)]
    ness = layer_runs("ne_10m_lakes", {"Loch Ness"})
    river_ness = layer_runs("ne_10m_rivers_lake_centerlines", {"Ness"})
    frame_low = to_ground(LON_MIN, LAT_MIN)
    frame_high = to_ground(LON_MAX, LAT_MAX)
    outlines = land_outlines()
    cities = {name: city_features(city) for name, city in CITIES.items()}
    norway, norway_shores = coast_land(NORWAY_BOX)

    places = titled_points([title for _, title in LANDMARKS] + CLYDE + FORTH + [t for titles in KELVIN for t in titles])
    clyde = [to_ground(*places[t][:2]) for t in CLYDE if t in places]
    forth = [to_ground(*places[t][:2]) for t in FORTH if t in places]
    kelvin = traced(KELVIN, places)

    lines = [
        "\\ Spitfire: the map, generated by tools/gen-spitfire-map.py from Natural",
        "\\ Earth's 1:10m vectors (public domain) and Wikidata (CC0); do not edit,",
        "\\ re-run the script. Metres east (x) and north (z) of 56°N 3.6°W.",
        "",
        "\\ the coastlines at sea level, each an array of [ x z ] points",
        "[",
    ]
    lines += ["  " + polyline_text(run) for run in coast]
    lines += ["] to coastlines", "",
              "\\ the lochs' shores, each closed, with the loch's surface height in metres",
              "{ :loch-lomond { :height 8 :shores ["]
    lines += ["    " + polyline_text(run) for run in lomond]
    lines += ["  ] }", "  :loch-ness { :height 16 :shores ["]
    lines += ["    " + polyline_text(run) for run in ness]
    lines += ["  ] }", "} to lochs", "",
              "\\ the rivers' courses, the Clyde and the Forth from their mouths, the Kelvin down to the Clyde",
              "{ :clyde " + polyline_text(clyde),
              "  :forth " + polyline_text(forth),
              "  :kelvin " + polyline_text(kelvin),
              "  :ness [ " + " ".join(polyline_text(run) for run in river_ness) + " ]",
              "} to rivers", "",
              "\\ the kilometre squares the countryside is laid out in, from the map's south-west corner",
              "{} constant land-origin-x".format(number(frame_low[0])),
              "{} constant land-origin-z".format(number(frame_low[1])),
              "{} constant land-cell".format(number(CELL_METRES)),
              "",
              "\\ the land's outlines within the map, each closed, the last point joined back to the first",
              "["]
    lines += ["  " + polyline_text(ring) for ring in outlines]
    lines += ["] to land-outlines", "",
              "\\ the map's edges, the frame the land's outlines are clipped to",
              polyline_text([frame_low, (frame_high[0], frame_low[1]), frame_high, (frame_low[0], frame_high[1])])
              + " to map-edges",
              "\\ England: land south of the map's southern edge, across its width",
              polyline_text([(frame_low[0], frame_low[1] - 500000), (frame_high[0], frame_low[1] - 500000),
                             (frame_high[0], frame_low[1]), frame_low]) + " to south-land", "",
              "\\ the land round the Trondheimsfjord, from OpenStreetMap's coastline, (c) OpenStreetMap",
              "\\ contributors under the ODbL: closed outlines within 63.25-63.80 N, 9.6-11.2 E, placed",
              "\\ by the azimuthal equidistant projection from 56 N 3.6 W, true in distance and bearing",
              "["]
    lines += ["  " + polyline_text(ring) for ring in norway]
    lines += ["] to norway-outlines",
              "\\ inland Norway east and south of that box, as two bands of land, their inner edges following the box's",
              "["]
    lines += ["  " + polyline_text(ring) for ring in norway_bands(NORWAY_BOX)]
    lines += ["] to norway-bands",
              "\\ the same coast's shore lines for drawing, pieces over 500 m, without the box's edges",
              "["]
    lines += ["  " + polyline_text(line) for line in norway_shores]
    lines += ["] to norway-shores", "",
              "\\ the landmarks as [ x z height ], height 0 where Wikidata gives none",
              "{"]
    for key, title in LANDMARKS:
        if title not in places:
            continue
        lon, lat, height = places[title]
        x, z = to_ground(lon, lat)
        lines.append("  :{} [ {} {} {} ]".format(key, number(x), number(z), number(height)))
    lines += ["} to landmarks"]
    for city, (streets, squares, stations, buildings, railways) in cities.items():
        lines += ["",
                  "\\ {} from OpenStreetMap, (c) OpenStreetMap contributors, under the ODbL:".format(city.capitalize()),
                  "\\ the main streets' centre lines, the squares' and the stations' outlines,",
                  "\\ and the main-line railways, each run of track once",
                  "{"]
        lines += ["  :{} {}".format(key, polyline_text(line)) for key, line in streets.items()]
        lines += ["}} to {}-streets".format(city), "{"]
        lines += ["  :{} {}".format(key, polyline_text(outline)) for key, outline in squares.items()]
        lines += ["}} to {}-squares".format(city), "{"]
        lines += ["  :{} {}".format(key, polyline_text(outline)) for key, outline in stations.items()]
        lines += ["}} to {}-stations".format(city), "{"]
        lines += ["  :{} {}".format(key, polyline_text(outline)) for key, outline in buildings.items()]
        lines += ["}} to {}-buildings".format(city), "["]
        lines += ["  " + polyline_text(line) for line in railways]
        lines += ["] to {}-railways".format(city)]
    lines += [""]
    with open(OUT, "w") as out:
        out.write("\n".join(lines))
    print("wrote", OUT, "coast runs", len(coast), "land outlines", len(outlines))


main()
