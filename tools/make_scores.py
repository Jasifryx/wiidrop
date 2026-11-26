"""Builds data/scores.tsv: Metacritic scores and RAWG user ratings for Wii and GameCube games, keyed by disc ID.
The data comes from RAWG (https://rawg.io) and is included in WiiDrop with RAWG's permission.

    python tools/make_scores.py            (asks for your RAWG API key; it isn't saved or printed)

Needs only Python 3. Downloads RAWG's Wii + GameCube game lists, plus GameTDB's title list to turn game names
into disc IDs, and writes one line per ID: ID <TAB> metacritic (0 = none) <TAB> RAWG rating x10 (0-50)."""
import getpass, json, os, re, sys, time, unicodedata, urllib.error, urllib.parse, urllib.request

UA = "WiiDrop-scores-builder/1.0"
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data", "scores.tsv")


def get(url, tries=6):
    for attempt in range(tries):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": UA})
            with urllib.request.urlopen(req, timeout=60) as r:
                return r.read()
        except urllib.error.HTTPError as e:
            if e.code == 401:
                sys.exit("RAWG refused the key (401). Check that you pasted your RAWG API key correctly.")
            if e.code in (429, 500, 502, 503, 504) and attempt < tries - 1:
                wait = 5 * (attempt + 1)
                print(f"  RAWG said {e.code}, waiting {wait} s...")
                time.sleep(wait)
                continue
            raise
        except urllib.error.URLError:
            if attempt < tries - 1:
                time.sleep(5)
                continue
            raise


def rawg(key, path, **params):
    params["key"] = key
    return json.loads(get("https://api.rawg.io/api/" + path + "?" + urllib.parse.urlencode(params)))


# ---- the same title rules WiiDrop uses (source/covers.c) ----
def norm(title):
    t = unicodedata.normalize("NFKD", title).encode("ascii", "ignore").decode()
    t = t.replace("&", "and").lower()
    t = re.sub(r"[^a-z0-9]", "", t)
    if t.startswith("the") and len(t) > 6:
        t = t[3:]
    return t[:63]


ROMAN = {"ii": "2", "iii": "3", "iv": "4"}
STOP = {"the", "a", "an", "of", "and"}


def words(title):
    t = unicodedata.normalize("NFKD", title).encode("ascii", "ignore").decode().lower().replace("'", "")
    return [ROMAN.get(w, w) for w in re.split(r"[^a-z0-9]+", t) if w and w not in STOP]


def close_match(a, b):
    """Dice >= 0.8 on words, at least 2 shared, and exactly the same numbers (sequels differ by number)."""
    wa, wb = words(a), words(b)
    if not wa or not wb:
        return 0
    nums = lambda ws: sorted(w for w in ws if w[0].isdigit())
    if nums(wa) != nums(wb):
        return 0
    common = 0
    left = list(wb)
    for w in wa:
        if w in left:
            left.remove(w)
            common += 1
    dice = 2 * common / (len(wa) + len(wb))
    return dice if common >= 2 and dice >= 0.8 else 0


def main():
    key = os.environ.get("RAWG_KEY") or getpass.getpass("RAWG API key (hidden): ").strip()
    if not key:
        sys.exit("No key given.")

    print("Finding RAWG's Wii and GameCube platform IDs...")
    plats, page = {}, 1
    while True:
        r = rawg(key, "platforms", page_size=50, page=page)
        for p in r["results"]:
            plats[p["name"]] = p["id"]
        if not r.get("next"):
            break
        page += 1
    wanted = {"Wii": plats.get("Wii"), "GameCube": plats.get("GameCube")}
    if not all(wanted.values()):
        sys.exit("Couldn't find the Wii / GameCube platforms on RAWG: " + repr(wanted))

    games = {"Wii": [], "GameCube": []}
    for name, pid in wanted.items():
        page = 1
        while True:
            r = rawg(key, "games", platforms=pid, page_size=40, page=page)
            for g in r["results"]:
                games[name].append((g.get("name") or "", g.get("metacritic") or 0, g.get("rating") or 0))
            print(f"  {name}: {len(games[name])} games", end="\r")
            if not r.get("next"):
                break
            page += 1
            time.sleep(0.25)
        print(f"  {name}: {len(games[name])} games")

    print("Downloading GameTDB's title list...")
    tdb = get("https://www.gametdb.com/wiitdb.txt?LANG=EN").decode("utf-8", "replace").splitlines()
    ids = []  # (id, title, platform)
    for line in tdb:
        m = re.match(r"^([A-Z0-9]{6}) = (.+)$", line.strip())
        if m:
            gid, title = m.group(1), m.group(2)
            ids.append((gid, title, "GameCube" if gid[0] in "GD" else "Wii"))

    out, matched_games = {}, 0
    for plat in ("Wii", "GameCube"):
        by_key = {}
        for gid, title, p in ids:
            if p == plat:
                by_key.setdefault(norm(title), []).append((gid, title))
        # word index, so a close match only compares titles that share a word
        index = {}
        for k, lst in by_key.items():
            for w in set(words(lst[0][1])):
                index.setdefault(w, set()).add(k)
        for name, meta, rating in games[plat]:
            if not meta and not rating:
                continue
            hits = by_key.get(norm(name))
            if not hits:  # close match, only if exactly one GameTDB title fits best
                best, bestKeys = 0, []
                cands = set()
                for w in set(words(name)):
                    cands |= index.get(w, set())
                for k in cands:
                    s = close_match(name, by_key[k][0][1])
                    if s > best + 1e-9:
                        best, bestKeys = s, [k]
                    elif s and abs(s - best) < 1e-9:
                        bestKeys.append(k)
                hits = by_key[bestKeys[0]] if len(bestKeys) == 1 else None
            if not hits:
                continue
            matched_games += 1
            m10 = int(meta) if meta and 0 < meta <= 100 else 0
            r10 = int(round(float(rating) * 10)) if rating else 0
            for gid, _ in hits:
                old = out.get(gid)
                if not old or (m10 and not old[0]):  # keep the entry that has a Metacritic score
                    out[gid] = (m10, r10)

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("# Metacritic scores and user ratings from RAWG (https://rawg.io), included with RAWG's permission.\n")
        f.write("# Built by tools/make_scores.py on %s. ID <TAB> metacritic (0 = none) <TAB> RAWG rating x10\n"
                % time.strftime("%Y-%m-%d"))
        for gid in sorted(out):
            f.write("%s\t%d\t%d\n" % (gid, out[gid][0], out[gid][1]))
    withMeta = sum(1 for v in out.values() if v[0])
    print(f"Matched {matched_games} RAWG games to {len(out)} disc IDs ({withMeta} with a Metacritic score).")
    print("Wrote", os.path.normpath(OUT))


if __name__ == "__main__":
    main()
