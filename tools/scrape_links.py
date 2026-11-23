#!/usr/bin/env python3
"""
Build a WiiDrop links.txt from your own website.

    python scrape_links.py https://example.com/my-dumps/
    python scrape_links.py https://example.com/my-dumps/ -o E:/wiiu/apps/wiidrop/links.txt
    python scrape_links.py https://example.com/my-dumps/ --ext iso,wbfs,rvz,zip,7z --depth 6
    python scrape_links.py https://example.com/wii/ https://example.com/gamecube/ https://other.site/dumps/
    python scrape_links.py --from sites.txt          (one listing page per line)
    python scrape_links.py https://example.com/new/ --append   (add to an existing links.txt)

(In Thonny's Shell, start the line with %Run instead of python.)

It crawls your site starting at that page: file listings, subfolders, "next page" links and
per-file pages with download buttons, several levels deep, loading 8 pages at a time. Every link to
a file ending in one of the extensions (default .iso, .wbfs, .rvz and .zip) ends up in links.txt. Download links
that don't show the file name (like /get?id=123) are recognised by asking the server what file they
deliver. It stays inside the folder you start in unless you add --anywhere, and never leaves your site.

Only uses Python's standard library.
"""
import argparse
import base64
import concurrent.futures as cf
import html
import re
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from html.parser import HTMLParser

USER_AGENT = "Mozilla/5.0 (compatible; WiiDrop-link-scraper/2.0)"

# file types that are never pages: don't bother loading them
SKIP_EXT = {
    "jpg", "jpeg", "png", "gif", "webp", "bmp", "ico", "svg", "css", "js", "mjs", "map", "woff", "woff2",
    "ttf", "otf", "eot", "mp3", "mp4", "mkv", "avi", "webm", "ogg", "wav", "flac", "pdf", "txt", "nfo", "md5",
    "sha1", "sfv", "json", "xml", "rss", "exe", "msi", "dmg", "apk", "rar", "gz", "bz2", "xz", "tar", "dll",
}
GENERIC_TEXT = re.compile(r"^\s*(download( now| file| link)?|get( it| file)?|here|click here|mirror\s*\d*|direct( link)?|"
                          r"link|file|start download|free download|\W*)\s*$", re.I)
DOWNLOADISH = re.compile(r"(download|/dl/|/dl\?|/get[/?]|getfile|file\.php|/file/|mirror|direct)", re.I)
BIG_FILE = 20 * 1024 * 1024


# ------------------------------------------------------------------ parsing

class PageParser(HTMLParser):
    """Collects every link-ish thing on a page: (url, link text)."""

    URL_ATTRS = ("href", "src", "data-href", "data-url", "data-src", "data-download", "data-link", "data-file",
                 "formaction", "action", "content")

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.links = []
        self.scripts = []  # contents of <script> tags
        self.texts = []    # visible text
        self._a = None
        self._text = []
        self._in_script = False

    def handle_starttag(self, tag, attrs):
        a = {k: (v or "") for k, v in attrs}
        if tag == "script":
            self._in_script = True
        if tag == "a" and "href" in a:
            self._a = a["href"]
            self._text = [a.get("title", ""), a.get("download", "")]
        for key in self.URL_ATTRS:
            v = a.get(key)
            if not v:
                continue
            if key == "href" and tag == "a":
                continue  # added with its text when the tag closes
            if key == "content":
                m = re.search(r"url\s*=\s*['\"]?([^'\";]+)", v, re.I)  # <meta http-equiv=refresh>
                if not (tag == "meta" and m):
                    continue
                v = m.group(1)
            if tag in ("img", "script", "link", "source", "video", "audio") and key in ("src", "href"):
                continue
            self.links.append((v, a.get("title", "") or a.get("aria-label", "")))
        for key in ("onclick", "onmousedown", "data-action"):
            js = a.get(key)
            if js:
                self.scripts.append(js)

    def handle_data(self, data):
        if self._in_script:
            self.scripts.append(data)
        else:
            self.texts.append(data)
        if self._a is not None:
            self._text.append(data)

    def handle_endtag(self, tag):
        if tag == "script":
            self._in_script = False
        if tag == "a" and self._a is not None:
            self.links.append((self._a, " ".join(" ".join(self._text).split())))
            self._a = None


def page_links(page, base, exts):
    p = PageParser()
    try:
        p.feed(page)
    except Exception:
        pass
    raw = list(p.links)
    js = "\n".join(p.scripts)
    text = "\n".join(p.texts)
    # absolute URLs written in scripts or shown as text
    raw += [(m.group(0), "") for m in re.finditer(r"""https?://[^\s"'<>\\)]+""", js + "\n" + text)]
    # quoted paths to our file types inside scripts:  "files/Game Name.iso", 'dl.php?f=X.wbfs'
    ext_re = "|".join(re.escape(e) for e in exts)
    raw += [(m.group(1), "") for m in re.finditer(
        r"""['"]([^'"<>\r\n]{1,300}?\.(?:%s)(?:[?#][^'"<>\r\n]*)?)['"]""" % ext_re, js, re.I)]
    # JS navigation:  location.href = '/file/12'   window.open("/dl?id=3")
    raw += [(m.group(1), "") for m in re.finditer(
        r"""(?:location(?:\.href)?\s*=|location\.(?:assign|replace)\s*\(|window\.open\s*\()\s*['"]([^'"]+)['"]""", js)]
    out = []
    for href, text in raw:
        href = html.unescape(href.strip()).replace("\\/", "/")
        if not href or href.startswith(("#", "javascript:", "mailto:", "tel:", "data:")):
            continue
        try:
            url = urllib.parse.urljoin(base, href)
        except ValueError:
            continue
        if url.startswith(("http://", "https://")):
            out.append((norm(urllib.parse.urldefrag(url)[0]), text))
    return out


def norm(url):
    """Percent-encode spaces and other unsafe characters (keeps existing %XX escapes)."""
    return urllib.parse.quote(url, safe=":/?#[]@!$&'()*+,;=%~")


def file_name_of(url):
    """The file name a link points at: last path part, or a ?file= / ?name= style value."""
    parts = urllib.parse.urlsplit(url)
    q = urllib.parse.parse_qs(parts.query)
    for key in ("file", "filename", "name", "f", "path", "download", "dl", "fn"):
        if q.get(key):
            return urllib.parse.unquote(q[key][0]).replace("\\", "/").rsplit("/", 1)[-1]
    return urllib.parse.unquote(parts.path.rstrip("/").rsplit("/", 1)[-1])


def ext_of(name):
    name = name.lower()
    return name.rsplit(".", 1)[-1] if "." in name else ""


def disposition_name(headers):
    cd = headers.get("Content-Disposition", "") or ""
    m = re.search(r"filename\*\s*=\s*[^']*'[^']*'([^;]+)", cd, re.I)
    if m:
        return urllib.parse.unquote(m.group(1).strip().strip('"'))
    m = re.search(r'filename\s*=\s*"?([^";]+)"?', cd, re.I)
    return m.group(1).strip() if m else ""


def sniff(head):
    """(extension, disc title) from the first bytes of a download, or ("", "")."""
    def title(raw):
        return " ".join(raw.split(b"\0", 1)[0].decode("ascii", "replace").split())
    if len(head) >= 0x260 and head[:4] == b"WBFS" and head[0x218:0x21C] == b"\x5d\x1c\x9e\xa3":
        return "wbfs", title(head[0x220:0x260])
    if len(head) >= 0x60 and head[0x18:0x1C] == b"\x5d\x1c\x9e\xa3":
        return "iso", title(head[0x20:0x60])
    if len(head) >= 0x60 and head[0x1C:0x20] == b"\xc2\x33\x9f\x3d":
        return "iso", title(head[0x20:0x60])
    if len(head) >= 0xD8 and head[:4] in (b"RVZ\x01", b"WIA\x01"):
        return "rvz", title(head[0x58 + 0x20:0x58 + 0x60])
    if head[:6] == b"7z\xbc\xaf\x27\x1c":
        return "7z", ""
    if head[:4] == b"PK\x03\x04":
        return "zip", ""
    return "", ""


def nice_name(file_name):
    """'Super%20Mario%20Galaxy%20(USA).iso' -> 'Super Mario Galaxy (USA)'"""
    base = urllib.parse.unquote(file_name)
    base = re.sub(r"\.(iso|wbfs|7z|zip|rar|ciso|wia|rvz)$", "", base, flags=re.I)
    base = base.replace("_", " ").replace("+", " ")
    return " ".join(base.split()) or file_name


# ------------------------------------------------------------------ crawler

class Crawler:
    def __init__(self, starts, exts, auth, depth, workers, max_pages, anywhere, verbose):
        self.exts = exts
        self.auth = auth
        self.depth = depth
        self.workers = workers
        self.max_pages = max_pages
        self.verbose = verbose
        self.starts = [norm(u) for u in starts]
        self.scopes = []  # (host, folder) per start page
        for u in self.starts:
            sp = urllib.parse.urlsplit(u)
            path = sp.path if sp.path.endswith("/") else sp.path.rsplit("/", 1)[0] + "/"
            self.scopes.append((sp.netloc.lower(), "/" if anywhere else urllib.parse.unquote(path)))
        self.hosts = {h for h, _ in self.scopes}
        self.found = {}  # download url -> file name
        self.seen = set()
        self.lock = threading.Lock()
        self.pages = 0
        self.errors = 0

    def in_scope(self, url):
        s = urllib.parse.urlsplit(url)
        host, path = s.netloc.lower(), urllib.parse.unquote(s.path)
        return any(host == h and path.startswith(prefix) for h, prefix in self.scopes)

    def key(self, url):
        """Normalise for the 'seen' set: drop Apache/nginx sort links and trailing index files."""
        s = urllib.parse.urlsplit(url)
        q = s.query
        if re.fullmatch(r"C=[NMSD](;|&)O=[AD]", q) or re.fullmatch(r"(sort|order|dir)=[\w-]+(&(sort|order|dir)=[\w-]+)*", q):
            q = ""
        path = re.sub(r"/(index\.(html?|php))$", "/", s.path)
        return urllib.parse.urlunsplit((s.scheme, s.netloc.lower(), path, q, ""))

    def add_found(self, url, name):
        with self.lock:
            if url not in self.found:
                self.found[url] = name
                if self.verbose:
                    print(f"  + {nice_name(name)}", file=sys.stderr)

    def request(self, url, method="GET"):
        req = urllib.request.Request(url, method=method, headers={"User-Agent": USER_AGENT, "Accept": "*/*"})
        if self.auth:
            req.add_header("Authorization", self.auth)
        return urllib.request.urlopen(req, timeout=25)

    def visit(self, url, hint, label, outside):
        """Load one URL. Returns new (url, text, hint, label, outside) links to crawl."""
        for attempt in range(2):
            try:
                with self.request(url) as r:
                    ctype = (r.headers.get("Content-Type") or "").lower()
                    final = r.geturl()
                    if "html" not in ctype and "xml" not in ctype and not ctype.startswith("text/"):
                        # it's a file: is it one of ours?
                        if ctype.startswith(("image/", "audio/", "video/", "font/")):
                            return []
                        size = int(r.headers.get("Content-Length") or 0)
                        named = [disposition_name(r.headers), file_name_of(final), file_name_of(url)]
                        # the link text is only trusted for a big binary download (not a thumbnail etc.)
                        if hint and (size == 0 or size >= BIG_FILE):
                            named.append(hint)
                        for name in named:
                            if name and ext_of(name) in self.exts:
                                self.add_found(url, name)
                                return []
                        # no telling name: look at the file itself (disc / archive signatures)
                        try:
                            head = r.read(0x300)
                        except Exception:
                            head = b""
                        kind, disc_title = sniff(head)
                        if kind in self.exts:
                            base = label or disc_title or file_name_of(url) or "game"
                            self.add_found(url, f"{base}.{kind}")
                        return []
                    page = r.read(8 * 1024 * 1024).decode(r.headers.get_content_charset() or "utf-8", "replace")
                break
            except urllib.error.HTTPError as e:
                if e.code in (429, 503) and attempt == 0:
                    time.sleep(2)
                    continue
                self.errors += 1
                return []
            except Exception:
                if attempt == 0:
                    continue
                self.errors += 1
                return []
        with self.lock:
            self.pages += 1

        out = []
        for link, text in page_links(page, final, self.exts):
            name = file_name_of(link)
            if ext_of(name) in self.exts:
                self.add_found(link, name)
                continue
            text_has_ext = ext_of(text) in self.exts
            s = urllib.parse.urlsplit(link)
            same_site = s.netloc.lower() in self.hosts
            if not same_site and not text_has_ext:
                continue  # never wander off to other sites
            if ext_of(name) in SKIP_EXT:
                continue
            if outside and not text_has_ext and not same_site:
                continue
            new_hint = text if text_has_ext else hint
            new_label = text if text and not GENERIC_TEXT.match(text) and not text_has_ext else label
            downloadish = bool(DOWNLOADISH.search(link)) or "download" in (text or "").lower()
            if outside:
                # a page outside the start folder (a per-file page): only follow its download links
                if text_has_ext or downloadish:
                    out.append((link, text, new_hint, new_label, True))
                continue
            out.append((link, text, new_hint, new_label, not self.in_scope(link)))
        return out

    def run(self):
        frontier = []
        for u in self.starts:
            k = self.key(u)
            if k not in self.seen:
                self.seen.add(k)
                frontier.append((u, "", "", "", False))
        t0 = time.time()
        with cf.ThreadPoolExecutor(max_workers=self.workers) as pool:
            for level in range(self.depth + 1):
                if not frontier or self.pages >= self.max_pages:
                    break
                print(f"level {level}: {len(frontier)} page(s) to check, {len(self.found)} links so far",
                      file=sys.stderr)
                futures = [pool.submit(self.visit, url, hint, label, outside) for url, _, hint, label, outside in frontier]
                nxt = []
                for f in cf.as_completed(futures):
                    for link, text, hint, label, outside in f.result():
                        k = self.key(link)
                        with self.lock:
                            if k in self.seen or link in self.found:
                                continue
                            self.seen.add(k)
                        nxt.append((link, text, hint, label, outside))
                room = self.max_pages - self.pages
                frontier = nxt[:max(room, 0)]
        return time.time() - t0


# ------------------------------------------------------------------ main

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("urls", nargs="*", metavar="url", help="one or more pages that list your files")
    ap.add_argument("--from", dest="from_file", help="text file with more listing pages, one per line")
    ap.add_argument("-o", "--output", default="links.txt", help="where to write (default: links.txt)")
    ap.add_argument("--append", action="store_true", help="add to the links already in the output file")
    ap.add_argument("--ext", default="iso,wbfs,rvz,zip", help="file types to collect (default: iso,wbfs,rvz,zip)")
    ap.add_argument("--depth", type=int, default=4, help="how many clicks deep to follow (default 4)")
    ap.add_argument("--workers", type=int, default=8, help="pages loaded at the same time (default 8)")
    ap.add_argument("--max-pages", type=int, default=5000, help="stop after this many pages (default 5000)")
    ap.add_argument("--anywhere", action="store_true", help="crawl the whole site, not just the start folder")
    ap.add_argument("--user", help="HTTP basic auth user (only used to read the pages, not saved)")
    ap.add_argument("--password", help="HTTP basic auth password")
    ap.add_argument("--no-names", action="store_true", help="write bare URLs without 'Name |'")
    ap.add_argument("-q", "--quiet", action="store_true", help="don't list links as they're found")
    a = ap.parse_args()

    starts = list(a.urls)
    if a.from_file:
        try:
            with open(a.from_file, encoding="utf-8-sig") as f:
                for line in f:
                    line = line.strip()
                    if line and not line.startswith("#"):
                        starts.append(line.split(" | ")[-1].strip())  # also accepts "Name | URL" lines
        except OSError as e:
            sys.exit(f"Couldn't read {a.from_file}: {e}")
    starts = [u if "://" in u else "https://" + u for u in starts]
    starts = list(dict.fromkeys(starts))  # drop duplicates, keep order
    if not starts:
        ap.error("give at least one listing page (or --from FILE)")

    exts = {e.strip().lower().lstrip(".") for e in a.ext.split(",") if e.strip()}
    auth = None
    if a.user:
        auth = "Basic " + base64.b64encode(f"{a.user}:{a.password or ''}".encode()).decode()

    if len(starts) > 1:
        print(f"crawling {len(starts)} sites/folders together", file=sys.stderr)
    c = Crawler(starts, exts, auth, max(a.depth, 0), max(a.workers, 1), a.max_pages, a.anywhere, not a.quiet)
    secs = c.run()
    if not c.found:
        sys.exit(f"No links found ({c.pages} pages checked, {c.errors} errors). Try --ext iso,wbfs,7z,zip, "
                 "--depth 8, --anywhere, or --user/--password if the site needs a login.")

    new = {url: (url if a.no_names else f"{nice_name(name)} | {url}") for url, name in c.found.items()}
    kept, comments = {}, []
    if a.append:
        try:
            with open(a.output, encoding="utf-8-sig") as f:
                for line in f:
                    line = line.rstrip("\r\n")
                    if not line.strip():
                        continue
                    if line.lstrip().startswith("#"):
                        comments.append(line)
                        continue
                    kept[line.split(" | ")[-1].strip()] = line  # keep the user's own names
        except OSError:
            pass  # nothing to append to yet
    added = [u for u in new if u not in kept]
    merged = dict(kept)
    for u in added:
        merged[u] = new[u]

    def sort_key(line):
        return (line.split(" | ")[0] if " | " in line else line).lower()

    with open(a.output, "w", encoding="utf-8", newline="\n") as f:
        for line in comments:
            f.write(line + "\n")
        for u in starts:
            f.write(f"# scraped from {u}\n")
        for line in sorted(merged.values(), key=sort_key):
            f.write(line + "\n")
    note = f", {len(kept)} already there" if a.append else ""
    print(f"Wrote {len(merged)} links to {a.output} ({len(added)} new{note}; {c.pages} pages checked in {secs:.1f}s"
          + (f", {c.errors} failed to load" if c.errors else "") + ")")


if __name__ == "__main__":
    main()
