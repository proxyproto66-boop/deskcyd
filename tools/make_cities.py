"""
Builds pc/cities.json.gz for the CYD's location picker from GeoNames
(cities with more than 15,000 people, about 30,000 places). Run by the GitHub build.

usage: python make_cities.py cities15000.txt admin1CodesASCII.txt countryInfo.txt out/cities.json.gz
GeoNames data: CC BY 4.0, https://www.geonames.org
"""
import gzip
import json
import sys
from collections import defaultdict

STATES_IF_MORE_THAN = 40   # countries with more cities than this get a State step
REGIONS = [("NA", "North America"), ("SA", "South America"), ("EU", "Europe"),
           ("AS", "Asia"), ("AF", "Africa"), ("OC", "Oceania")]


def ascii_only(s):
    return s.encode("ascii", "ignore").decode().strip()


def main():
    cities_txt, admin1_txt, country_txt, out = sys.argv[1:5]

    countries = {}   # code -> (name, continent)
    for line in open(country_txt, encoding="utf-8"):
        if line.startswith("#") or not line.strip():
            continue
        f = line.rstrip("\n").split("\t")
        if len(f) > 8:
            countries[f[0]] = (ascii_only(f[4]), f[8])

    admin1 = {}      # "US.MA" -> "Massachusetts"
    for line in open(admin1_txt, encoding="utf-8"):
        f = line.rstrip("\n").split("\t")
        if len(f) >= 3:
            admin1[f[0]] = ascii_only(f[2]) or ascii_only(f[1])

    by_country = defaultdict(list)
    for line in open(cities_txt, encoding="utf-8"):
        f = line.rstrip("\n").split("\t")
        if len(f) < 15:
            continue
        name = ascii_only(f[2]) or ascii_only(f[1])
        if not name:
            continue
        by_country[f[8]].append((name[:30], round(float(f[4]), 3), round(float(f[5]), 3), f[10]))

    data = {"regions": [], "countries": {}, "states": {}, "cities": {}}
    for rcode, rname in REGIONS:
        clist = []
        for cc, (cname, cont) in countries.items():
            if cont != rcode or cc not in by_country:
                continue
            places = by_country[cc]
            use_states = len(places) > STATES_IF_MORE_THAN
            clist.append([cname, cc, 1 if use_states else 0])
            if use_states:
                groups = defaultdict(list)
                for name, lat, lon, a1 in places:
                    groups[a1 if f"{cc}.{a1}" in admin1 else "_"].append([name, lat, lon])
                slist = []
                for a1, items in groups.items():
                    sname = admin1.get(f"{cc}.{a1}", "Other")
                    slist.append([sname, a1])
                    data["cities"][f"{cc}.{a1}"] = sorted(items, key=lambda c: c[0].lower())
                data["states"][cc] = sorted(slist, key=lambda s: (s[0] == "Other", s[0].lower()))
            else:
                data["cities"][cc] = sorted(([n, la, lo] for n, la, lo, _ in places), key=lambda c: c[0].lower())
        if clist:
            data["regions"].append([rname, rcode])
            data["countries"][rcode] = sorted(clist, key=lambda c: c[0].lower())

    raw = json.dumps(data, separators=(",", ":")).encode()
    with gzip.open(out, "wb", compresslevel=9) as fh:
        fh.write(raw)
    total = sum(len(v) for v in data["cities"].values())
    biggest = max(len(v) for v in data["cities"].values())
    print(f"wrote {out}: {total} cities, largest list {biggest}, {len(raw)} bytes raw")


if __name__ == "__main__":
    main()
