# WiiDrop

Wii U homebrew (Aroma) that downloads your own Wii and GameCube dumps **straight from direct links**
onto the SD card, ready for vWii loaders. No PC or server program needed: the console fetches each link itself.

| Link points to        | Lands on the SD card                                   | Launch with             |
|-----------------------|--------------------------------------------------------|-------------------------|
| a Wii `.wbfs`         | `sd:/wbfs/<Title> [ID]/ID.wbfs` (+ `.wbf1`… if > 4 GB) | USB Loader GX / WiiFlow |
| a Wii `.iso`          | same, **converted to WBFS while it downloads**         | USB Loader GX / WiiFlow |
| a GameCube `.iso`     | `sd:/games/<Title> [ID]/game.iso` (disc 2: `disc2.iso`) | Nintendont             |
| a Wii or GameCube `.rvz` (Dolphin) | same as above: rebuilt, then WBFS (Wii) or ISO (GC) | |
| a `.7z` or `.zip` with any of these | same as above (only the game is extracted)             |                         |

The game's ID, title and type come from the file's own disc header, so all you supply is the link.

Features
- Games over 4 GB are split automatically for FAT32 (`ID.wbfs`, `ID.wbf1`…, same scheme as Wii Backup Manager).
- Checks the first bytes of every download. A link that returns a web page (a typical share-link problem)
  is stopped right away with a clear message instead of filling your card.
- Downloads are staged in `sd:/wiiu/apps/wiidrop/tmp/` and moved into place only when complete.
- **Fast downloads:** big files come down over 4 connections at once (4 MB pieces, handed to the installer in
  order), because a single Wii U connection is slow. Set `connections = 1` in config.ini for hosts that only
  allow one connection. Hosts that don't support Range requests automatically get a single connection.
- **Resume + auto-retry:** press B to pause, or lose Wi-Fi: WiiDrop waits (5 s up to 60 s), reconnects and
  continues where it stopped, up to 20 times. Picking a paused game again also continues it.
- **Wii ISO -> WBFS on the fly:** a Wii `.iso` (direct or inside a `.7z`) is converted to WBFS as it streams in,
  in one pass, so no PC tool and no double SD space. The empty padding outside the disc's partitions is left
  out, so the WBFS is usually smaller than the ISO. The format matches what USB Loader GX makes itself (2 MB
  blocks, trimmed header) and was checked by reading the output back with USB Loader GX's own libwbfs code.
  Pausing / losing Wi-Fi resumes from the last 16 MB checkpoint.
- **RVZ (Dolphin's compressed format):** the console downloads the .rvz, then rebuilds the original disc byte for
  byte: Zstandard / LZMA / LZMA2 / bzip2 / uncompressed, any chunk size, plus Dolphin's older WIA format. Wii
  partitions are re-hashed and re-encrypted with the title key the RVZ itself stores, so no console keys are
  needed. Wii games come out as WBFS (converted on the fly), GameCube games as ISO for Nintendont. Checked
  against 24 RVZ/WIA files made by Dolphin's own converter. Needs SD space for the .rvz and the game at once;
  rebuilding Wii games is CPU-heavy (hashing + AES), so it runs on all three of the Wii U's CPU cores. A `.zip`
  with an `.rvz` inside works too: the .rvz is unzipped while it downloads, then rebuilt.
- **.zip archives:** unzipped *while downloading*: the zip never touches the SD card, and a Wii ISO inside is
  converted to WBFS in the same pass, so you only need space for the finished game. Supports Deflate and
  stored entries, zip64 (files over 4 GB) and streamed zips. Pausing resumes from a saved point inside the
  compressed data (zlib's zran technique). Readme/nfo files are skipped, and the CRC is checked.
- **.7z archives:** the console downloads the archive, streams the `.wbfs`/`.iso` out of it straight to the SD
  card (never into RAM), names it after the title ID, and deletes the archive. Anything else in the archive
  (readme `.txt`, `.nfo`…) is skipped. The CRC is verified.
- **GUI:** animated, GPU-rendered interface (SDL2) on TV and GamePad, using the Wii U's own system font.
  Touch works too (including the search keyboard), and so do Wii Remotes, Classic Controllers and Pro Controllers.
- **Game details:** cover, game ID, type, region, size, where it's installed, and actions (install, queue, star,
  delete).
- **Download queue:** queue games with **Y**, then *Download the queue* in the **+** menu installs them one
  after another, with the time left for the whole queue. Problems are collected into one summary at the end; B
  pauses the queue. Kept across restarts. With *Show: Queued*, the list is the queue and **L/R** move the selected
  game up or down. Optional: turn the Wii U off when the queue is done (+ menu), and a chime when a download or
  the queue finishes (*Sounds*).
- **Two-disc GameCube games:** disc 2 goes next to disc 1 as `disc2.iso`, the way Nintendont expects.
- **Manage games:** delete an installed game, or discard a partial download, from its details screen (**-**).
  *SD card: installed games* in the + menu lists every Wii and GameCube game on the card (also ones copied
  over from a PC) with its size and the space left, and deletes any of them.
- **Updates:** at start WiiDrop looks for a newer release on GitHub and asks before updating. The download is
  checked against GitHub's SHA-256 before it replaces the app. Turn it off with `update_check = 0`.
- **Sort, filter, star:** sort by list order / A-Z / installed first; show all, starred, installed, not
  installed or queued games; show only one region (USA / Europe / Japan, from the name's Redump tags or the
  game ID; games whose region can't be told stay visible); hide region duplicates (keeps the installed copy,
  else your preferred region).
- **Checksums (optional):** add `| md5=...`, `| sha1=...` or `| crc32=...` after a link and WiiDrop checks the
  download against it before installing.
- **Your list from your server:** set `links_url` in config.ini and WiiDrop loads the list from there each start
  (the last copy is kept for when the server can't be reached).
- **Cover art:** box art from [GameTDB](https://www.gametdb.com) appears on each card. For `.wbfs`/`.iso`/`.rvz`
  links WiiDrop reads just the first bytes of the file from your server to learn the game ID and real title before
  you install anything; for a `.zip` it unpacks the start of the game inside the first 64 KB. If that doesn't work
  (a `.7z`, say), it matches the file name against GameTDB's title list, so `Legend of Zelda, The - Twilight
  Princess (USA).7z` finds RZDE01 (the region tag picks the version). Close names work too (`Disney-Pixar Toy
  Story 3`, `super_mario_galaxy_2.zip`). If a game still has no ID, its details screen says why.
  Covers are cached in `sd:/wiiu/apps/wiidrop/covers/` and, if USB Loader GX is installed, copied to its
  `images/2D/` folder too. Loading happens in the background, so the menu never freezes.
- **Menu music:** an original, minimal dark-ambient piece that WiiDrop composes live (`source/bgm.c`: a low
  drone with slow held tones fading in and out), so it never loops exactly and there's no audio file at all. Turn it on/off in the
  + menu.
  Drop your own `music.mp3` in `sd:/wiiu/apps/wiidrop/` to replace it, or set `music = 0` in config.ini to start muted.
- Free-space and size checks, optional HTTP basic auth, http and https.

## Build

### Option A: GitHub Actions (nothing to install)
Push this folder to a GitHub repo. `.github/workflows/build.yml` builds it in the official devkitPro
container. Download `wiidrop.wuhb` from the run's **Artifacts**.

### Option B: locally on Windows
1. Install devkitPro with the [devkitPro installer](https://github.com/devkitPro/installer/releases) and tick **Wii U Development**.
2. In the devkitPro **MSYS2** shell:
   ```sh
   dkp-pacman -Syu --needed wut wut-tools wiiu-curl wiiu-sdl2 wiiu-sdl2_ttf wiiu-sdl2_mixer wiiu-sdl2_image
   cd /c/path/to/wiidrop
   make
   ```

The Wii U Menu icon is `meta/icon.png` (redraw it with `python tools/make_icon.py`).

## Use

1. Copy `wiidrop.wuhb` to `sd:/wiiu/apps/` and launch it once. It creates
   `sd:/wiiu/apps/wiidrop/links.txt` and `config.ini`.
2. Add your links to `links.txt`, one per line, with an optional name in front:
   ```
   Mario Kart Wii | https://example.com/my-dumps/RMCE01.wbfs
   https://example.com/my-dumps/Melee.iso
   Smash Brawl | https://example.com/my-dumps/Brawl.7z
   Zelda TP | https://example.com/my-dumps/RZDE01.wbfs | md5=0123456789abcdef0123456789abcdef
   ```
   The checksum at the end is optional (md5, sha1 or crc32 of the file the link downloads).
3. In WiiDrop:

   | Button | List | Details screen |
   |---|---|---|
   | **A** (or tap) | open the game's details | install / resume / reinstall |
   | **Y** | add to / remove from the queue | same |
   | **X** | star / unstar | same |
   | **-** | search (Wii U keyboard; every word must match, e.g. `mario usa`) | delete the game / discard a partial download |
   | **B** | clear the search | back |
   | **+** | menu: download the queue, sort, show, region duplicates, music, reload, exit | |
   | **L/R**, **ZL/ZR** | previous/next letter, 100 titles up/down (in *Show: Queued*, L/R move the game in the queue) | |

   During a download **B** pauses (progress is kept). HOME is disabled only while a download is running.
   Wii Remote: 1 = X, 2 = Y. Classic / Pro Controller buttons work like the GamePad's.

### Making links.txt from your website
If your site lists your files on a page, `tools/scrape_links.py` (Python 3, nothing to install) builds the
whole links.txt for you. It crawls 8 pages at a time through file listings, subfolders, "next page" links
and per-file pages with download buttons, and also finds download links built in JavaScript. Links that
don't show a file name (like `/get?id=123`) are identified from the server's reply or the file's own
disc/archive header. It stays inside the folder you start in and never leaves your site:

```powershell
python tools\scrape_links.py https://example.com/my-dumps/ -o E:\wiiu\apps\wiidrop\links.txt
```

By default it collects `.iso`, `.wbfs`, `.rvz` and `.zip`. Options: `--ext iso,wbfs,rvz,zip,7z` to add .7z, `--depth 8` to dig
deeper (default 4 clicks), `--anywhere` to crawl the whole site instead of just the start folder,
`--workers 16` for more parallel loads, `--user`/`--password` for a login, and `--no-names` for bare URLs.
Several sites or folders at once: list them all on one line (`... https://site/wii/ https://site/gc/
https://other.site/dumps/`) or put them in a text file, one per line, and use `--from sites.txt`. They're
crawled together into one sorted links.txt. Add `--append` to add new finds to an existing links.txt
(your own names, comments and hand-added links are kept). Run it again whenever you add games.

### Link requirements
- It must be a **direct** link: opening it should start the download, not show a page.
  Dropbox works with `?dl=1`. Google Drive / OneDrive / Mega share pages usually don't, because they show
  a confirmation page for large files.
- Wii games can be `.wbfs`, `.iso` or `.rvz`; GameCube games `.iso` or `.rvz` (everything is converted on the
  console). `.ciso` and `.gcz` aren't supported.
- `.7z` archives: made with 7-Zip's normal settings (LZMA2 or LZMA, solid or not) work. Extracting needs free
  space for the archive and the game at the same time; the archive is deleted once the game is in place.
  Archives made with a dictionary over 512 MB (only possible with custom settings) are too big for the
  Wii U's memory. Extraction speed is limited by the console's CPU, so expect a few extra minutes per game.
- Don't link split `.wbf1` parts. Link one whole `.wbfs` and WiiDrop splits it on the console.

### config.ini (optional)
```ini
user =          # basic auth for your host, if it needs one
password =
ca_file =       # your own certificate / CA on the SD card (for a self-signed server)
insecure = 0    # 1 = don't check https certificates at all
music = 1       # 0 = start muted
connections = 4 # parallel connections per download (1-8); 1 for hosts that limit connections
links_url =     # load links.txt from this URL instead (the last copy is kept on the SD card)
region = USA    # preferred region for "hide region duplicates": USA, Europe or Japan
update_check = 1 # look for a new WiiDrop on GitHub at start (always asks first)
```
HTTPS certificates are checked against Mozilla's list of trusted authorities, which is built into WiiDrop
(`data/cacert.pem`, from curl.se). Servers with a normal certificate (Let's Encrypt etc.) just work. For your own
server with a self-signed certificate, copy its certificate to the SD card and set
`ca_file = fs:/vol/external01/wiiu/apps/wiidrop/myserver.pem`, or set `insecure = 1` (still encrypted, but
someone on your network could impersonate the server). Plain `http://` links aren't encrypted at all.
The password is stored in plain text on the SD card.

## Layout

```
source/main.c         app flow: input, music, staging, install (SDL owns ProcUI / exiting)
source/ui.c           the GUI, pure SDL2 + SDL2_ttf (also builds on PC for previews)
source/net.c          libcurl: resumable streaming download, over several connections at once
source/rvz.c          RVZ/WIA reader: rebuilds the disc (zstd/LZMA/bzip2, junk regeneration, re-hash + AES on 3 cores)
source/wiicrypto.c    SHA-1 + AES-128 used for Wii partitions
source/zstd/          Zstandard 1.5.7 decompressor (BSD), unmodified
source/wbfsconv.c     streaming Wii ISO -> WBFS converter with resume checkpoints
source/disc.c         identify WBFS / GameCube ISO / Wii ISO from header bytes
source/splitwriter.c  FAT32 4 GB splitting + resume bookkeeping
source/covers.c       background worker: header / zip peek or GameTDB title match for IDs, cover downloads
source/zipstream.c    streaming .zip reader with resume points (zlib inflate)
source/sevenzip.c     streaming .7z extraction (reads split staging files, writes split output)
source/lzma/          7-Zip 26.06 C decoder by Igor Pavlov (public domain), unmodified
source/config.c       links.txt + config.ini parsing
```

## Music

The music is generated by `source/bgm.c` while the app runs: original, part of WiiDrop, no samples or audio files.
To hear it on a PC, build `tools/bgm_preview.c` (instructions at the top of the file) and run
`bgm_preview out.wav 120`. Your own `sd:/wiiu/apps/wiidrop/music.mp3` replaces it on the console.

## Legal

WiiDrop is a download manager and format converter. It contains no games, no game links, no Nintendo keys or
code, and no copyrighted audio or artwork; the box art is downloaded from GameTDB on your console at runtime,
like USB Loader GX does. It's meant for installing backups of games you own, from your own server. Whether
making or downloading such backups is legal depends on where you live, and what you download is your
responsibility.

WiiDrop is not affiliated with or endorsed by Nintendo. Wii, Wii U and GameCube are trademarks of Nintendo.

## License

WiiDrop is free software under the [GNU General Public License v3.0](LICENSE) or (at your option) any later
version (`GPL-3.0-or-later`). You may use, share and change it; if you share a changed version, you must share
its source code under the same license. The libraries it uses keep their own licenses
([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)), all of which can be combined with GPL-3.0.

## Credits

- Box art and the title list: [GameTDB](https://www.gametdb.com).
- Libraries: see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Checking an RVZ on your PC

`tools/rvz2iso` is a Windows build of the same RVZ code (build it with `tools\rvz2iso\build.bat`):

```powershell
tools\rvz2iso\rvz2iso.exe "Some Game.rvz" --compare "Some Game.iso"
```

Make the ISO with Dolphin (right-click the game > Convert File > ISO); it should say `MATCH: byte-identical`.
`rvz2iso.exe game.rvz game.iso` rebuilds an ISO directly, and `rvz2iso.exe --selftest` checks the crypto.
