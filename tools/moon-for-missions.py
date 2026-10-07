#!/usr/bin/env python3
"""Print the Moon as seen at each of the Spitfire's night missions: its
azimuth and altitude (degrees, topocentric), its apparent radius (degrees),
the share of its disc lit, and the position angle of its bright limb
measured from the direction to the zenith (degrees, toward the east).

Maintainer tool, run by hand; the figures are copied into
examples/spitfire/enemies.telic's missions. The method is Paul Schlyter's
"How to compute planetary positions" (stjarnhimlen.se/comp/ppcomp.html):
the Sun's and the Moon's orbital elements, the Moon's twelve largest
perturbations in longitude, five in latitude and two in distance, the
conversion to equatorial and horizontal coordinates, and the topocentric
correction to altitude; accurate to 1-2 arc minutes. The bright limb's
angle is the angle from the Moon toward the Sun on the sky, from the two
bodies' horizontal coordinates.

  python3 tools/moon-for-missions.py
"""

import math

MISSIONS = [
    ("Clydebank, 13 March 1941, 21:15 BST = 20:15 UT", 1941, 3, 13, 20.25, 55.902, -4.405),
    ("Greenock, 7 May 1941, 00:15 BDST = 6 May 22:15 UT", 1941, 5, 6, 22.25, 55.948, -4.765),
]


def rev(x):
    return x % 360.0


def sind(x):
    return math.sin(math.radians(x))


def cosd(x):
    return math.cos(math.radians(x))


def atan2d(y, x):
    return math.degrees(math.atan2(y, x))


def day_number(y, m, day, ut):
    return 367 * y - 7 * (y + (m + 9) // 12) // 4 + 275 * m // 9 + day - 730530 + ut / 24.0


def sun(d):
    w = 282.9404 + 4.70935e-5 * d
    e = 0.016709 - 1.151e-9 * d
    m = rev(356.0470 + 0.9856002585 * d)
    big_e = m + math.degrees(e) * sind(m) * (1 + e * cosd(m))
    xv = cosd(big_e) - e
    yv = math.sqrt(1 - e * e) * sind(big_e)
    v = atan2d(yv, xv)
    return rev(v + w), m, w


def moon(d, sun_m, sun_w):
    n = rev(125.1228 - 0.0529538083 * d)
    i = 5.1454
    w = rev(318.0634 + 0.1643573223 * d)
    a = 60.2666
    e = 0.054900
    m = rev(115.3654 + 13.0649929509 * d)
    big_e = m + math.degrees(e) * sind(m) * (1 + e * cosd(m))
    for _ in range(10):
        big_e = big_e - (big_e - math.degrees(e) * sind(big_e) - m) / (1 - e * cosd(big_e))
    xv = a * (cosd(big_e) - e)
    yv = a * math.sqrt(1 - e * e) * sind(big_e)
    v = atan2d(yv, xv)
    r = math.hypot(xv, yv)
    xh = r * (cosd(n) * cosd(v + w) - sind(n) * sind(v + w) * cosd(i))
    yh = r * (sind(n) * cosd(v + w) + cosd(n) * sind(v + w) * cosd(i))
    zh = r * sind(v + w) * sind(i)
    lon = atan2d(yh, xh)
    lat = atan2d(zh, math.hypot(xh, yh))
    ls = sun_m + sun_w
    lm = m + w + n
    dd = lm - ls
    f = lm - n
    lon += (-1.274 * sind(m - 2 * dd) + 0.658 * sind(2 * dd) - 0.186 * sind(sun_m)
            - 0.059 * sind(2 * m - 2 * dd) - 0.057 * sind(m - 2 * dd + sun_m) + 0.053 * sind(m + 2 * dd)
            + 0.046 * sind(2 * dd - sun_m) + 0.041 * sind(m - sun_m) - 0.035 * sind(dd)
            - 0.031 * sind(m + sun_m) - 0.015 * sind(2 * f - 2 * dd) + 0.011 * sind(m - 4 * dd))
    lat += (-0.173 * sind(f - 2 * dd) - 0.055 * sind(m - f - 2 * dd) - 0.046 * sind(m + f - 2 * dd)
            + 0.033 * sind(f + 2 * dd) + 0.017 * sind(2 * m + f))
    r += -0.58 * cosd(m - 2 * dd) - 0.46 * cosd(2 * dd)
    return rev(lon), lat, r


def equatorial(lon, lat, ecl):
    xg = cosd(lon) * cosd(lat)
    yg = sind(lon) * cosd(lat)
    zg = sind(lat)
    xe = xg
    ye = yg * cosd(ecl) - zg * sind(ecl)
    ze = yg * sind(ecl) + zg * cosd(ecl)
    return rev(atan2d(ye, xe)), atan2d(ze, math.hypot(xe, ye))


def horizontal(ra, dec, lst, latitude):
    ha = lst - ra
    x = cosd(ha) * cosd(dec)
    y = sind(ha) * cosd(dec)
    z = sind(dec)
    xhor = x * sind(latitude) - z * cosd(latitude)
    zhor = x * cosd(latitude) + z * sind(latitude)
    return rev(atan2d(y, xhor) + 180), math.degrees(math.asin(max(-1, min(1, zhor))))


def main():
    for name, y, m, day, ut, latitude, longitude in MISSIONS:
        d = day_number(y, m, day, ut)
        ecl = 23.4393 - 3.563e-7 * d
        sun_lon, sun_m, sun_w = sun(d)
        moon_lon, moon_lat, moon_r = moon(d, sun_m, sun_w)
        lst = rev(sun_m + sun_w + 180 + ut * 15 + longitude)
        sun_ra, sun_dec = equatorial(sun_lon, 0, ecl)
        moon_ra, moon_dec = equatorial(moon_lon, moon_lat, ecl)
        moon_az, moon_alt = horizontal(moon_ra, moon_dec, lst, latitude)
        moon_alt -= math.degrees(math.asin(1 / moon_r)) * cosd(moon_alt)
        sun_az, sun_alt = horizontal(sun_ra, sun_dec, lst, latitude)
        elongation = math.degrees(math.acos(cosd(sun_lon - moon_lon) * cosd(moon_lat)))
        lit = (1 - cosd(elongation)) / 2
        radius = 1873.7 * 60 / moon_r / 3600 / 2
        toward_sun_up = sind(sun_alt) * cosd(moon_alt) - cosd(sun_alt) * sind(moon_alt) * cosd(sun_az - moon_az)
        toward_sun_east = cosd(sun_alt) * sind(sun_az - moon_az)
        limb = rev(atan2d(toward_sun_east, toward_sun_up))
        print(name)
        print("  moon azimuth {:.1f} altitude {:.1f} radius {:.3f} lit {:.3f} bright limb {:.1f}".format(
            moon_az, moon_alt, radius, lit, limb))
        print("  sun azimuth {:.1f} altitude {:.1f}".format(sun_az, sun_alt))


main()
