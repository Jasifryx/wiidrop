/*
 * WiiDrop - download Wii / GameCube dumps straight from direct links to the SD card.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Put links (to .wbfs, GameCube .iso, or .7z archives of them) in sd:/wiiu/apps/wiidrop/links.txt.
 * The console downloads each one directly and works out what it is from the file's own header:
 *
 *   Wii  (.wbfs) -> sd:/wbfs/<Title> [<ID>]/<ID>.wbfs (+ .wbf1, .wbf2 ... split for FAT32)  [USB Loader GX / WiiFlow]
 *   GC   (.iso)  -> sd:/games/<Title> [<ID>]/game.iso                                       [Nintendont]
 *
 * Downloads are staged in sd:/wiiu/apps/wiidrop/tmp/<hash>/ and only moved into place when complete,
 * so loaders never see half-written games. Interrupted downloads resume where they stopped.
 *
 * GUI and music: SDL2 (ui.c). SDL's Wii U backend also owns ProcUI (HOME menu, exiting): we never
 * touch ProcUI ourselves, and quitting goes through SDL_Quit(), which returns to the Wii U Menu.
 */
#include <SDL.h>
#include <SDL_mixer.h>
#include <SDL_image.h>
#include <SDL_syswm.h>
#include <SDL_ttf.h>
#include <strings.h>

#include <coreinit/im.h>
#include <coreinit/launch.h>
#include <padscore/kpad.h>
#include <padscore/wpad.h>
#include <mbedtls/md5.h>
#include <mbedtls/sha1.h>
#include <mbedtls/sha256.h>
#include <zlib.h>
#include <coreinit/memory.h>
#include <coreinit/time.h>
#include <coreinit/systeminfo.h>
#include <vpad/input.h>

#include <dirent.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "config.h"
#include "covers.h"
#include "disc.h"
#include "net.h"
#include "rvz.h"
#include "sevenzip.h"
#include "splitwriter.h"
#include "ui.h"
#include "usbfs.h"
#include "wbfsconv.h"
#include "zipstream.h"


#define SD_ROOT        "fs:/vol/external01"
#define APP_DIR        SD_ROOT "/wiiu/apps/wiidrop"
#define CONFIG_PATH    APP_DIR "/config.ini"
#define LINKS_PATH     APP_DIR "/links.txt"
#define INSTALLED_PATH APP_DIR "/installed.txt"
#define MUSIC_PATH     APP_DIR "/music.mp3"
#define TMP_DIR        APP_DIR "/tmp"
#define COVER_DIR      APP_DIR "/covers"
#define FAV_PATH       APP_DIR "/favorites.txt"
#define QUEUE_PATH     APP_DIR "/queue.txt"
#define STATE_PATH     APP_DIR "/state.ini"
#define REMOTE_LINKS   APP_DIR "/links_remote.txt"
#define USB_ROOT       "usb:"
#define USB_TMP        USB_ROOT "/wiidrop/tmp"
#define USBGX_ROOT     SD_ROOT "/apps/usbloader_gx"
#define USBGX_COVERS   USBGX_ROOT "/images/2D"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define MUSIC_MAX_VOLUME 84 /* of 128 */
#define WIIDROP_VERSION  "1.0.0-beta.5"

/* ------------------------------------------------------------------ app state */

static SDL_Window *gWindow;
static SDL_Renderer *gRenderer;
static bool gQuit;           /* SDL_QUIT received (HOME menu -> Close) */
static bool gVisible = true; /* false while another app/HOME menu owns the screen */
static char gSdText[48];
static int gSel; /* position in the visible (possibly filtered) list */
static Config gCfg;

/* where games go: the SD card, or a FAT32 USB drive (usb:/, see usbfs.c). Downloads are staged on the same
   drive, so finished games are moved into place, never copied. */
static bool gUseUsb;
static bool gUsbSeen;            /* a USB drive was used before: mount it at start to see what's on it */
static char gTmpDir[64] = TMP_DIR;
static char gStartNote[300];     /* shown once after start (the USB drive couldn't be used) */

static const char *game_root(bool usb)
{
    return usb ? USB_ROOT : SD_ROOT;
}

static const char *drive_prefix(bool usb) /* for messages */
{
    return usb ? "usb:" : "sd:";
}
static char gListNote[300]; /* shown once after loading (e.g. links_url unreachable) */

/* list options (+ menu), saved in state.ini */
enum { SORT_LIST, SORT_AZ, SORT_INSTALLED, SORT_META, SORT_COUNT };
enum { SHOW_ALL, SHOW_FAV, SHOW_INSTALLED, SHOW_NOT_INSTALLED, SHOW_QUEUED, SHOW_COUNT };
static int gSort, gShow;
static bool gHideDupes;
static bool gQueueOff;   /* turn the Wii U off when the queue finishes */
static bool gChimeOn = true; /* a short chime when a download / the queue is done */
static char gRegionPref = 'E'; /* E / P / J: which copy "hide region duplicates" keeps */
static char gRegionOnly;        /* 0 = all regions, else E / P / J: hide games from other regions */

/* queue mode: errors are collected instead of shown one by one */
static bool gBatch;
static char gBatchErr[400];

/* Search: the Wii U software keyboard sets gQuery; the list then shows only matching titles.
   items[] stays indexed by entry; gView maps visible positions to entries (and gPosOf back). */
static char gQuery[128];
static bool gSearchOpen;
static double gSearchOpened;
static char gSearchBuf[512];

static Mix_Music *gMusic;     /* your own sd:/wiiu/apps/wiidrop/music.mp3 (there's no built-in music) */
static int gMusicState = -1; /* -1 none, 0 muted, 1 playing */
static float gMusicVol, gMusicTarget;
static double gMusicLast;

static double now_s(void)
{
    return SDL_GetTicks64() / 1000.0;
}

static void fmt_size(char *out, size_t n, uint64_t b)
{
    if (b >= 1024ULL * 1024 * 1024) snprintf(out, n, "%.1f GB", b / (1024.0 * 1024 * 1024));
    else if (b >= 1024ULL * 1024) snprintf(out, n, "%.0f MB", b / (1024.0 * 1024));
    else snprintf(out, n, "%" PRIu64 " KB", b / 1024);
}

/* ------------------------------------------------------------------ music */

static void music_init(bool startOn)
{
    if (Mix_OpenAudio(48000, AUDIO_S16SYS, 2, 2048) != 0) return;
    Mix_Init(MIX_INIT_MP3);

    struct stat st;
    if (stat(MUSIC_PATH, &st) == 0) gMusic = Mix_LoadMUS(MUSIC_PATH); /* the user's own track wins */
    if (!gMusic) return; /* no built-in music: only your own track (the menu hides the toggle) */

    gMusicState = startOn ? 1 : 0;
    gMusicVol = 0;
    gMusicTarget = startOn ? 1 : 0;
    if (gMusic) {
        Mix_VolumeMusic(0);
        Mix_PlayMusic(gMusic, -1);
        if (!startOn) Mix_PauseMusic();
    }
    gMusicLast = now_s();
}

/* Smooth fades for mute/unmute; called every frame from pump(). */
static void music_update(void)
{
    if (!gMusic) return;
    double t = now_s();
    float dt = (float)(t - gMusicLast);
    gMusicLast = t;
    float step = dt * (gMusicTarget > gMusicVol ? 0.6f : 2.5f); /* slow fade in, quick fade out */
    if (gMusicVol < gMusicTarget) gMusicVol = gMusicVol + step > gMusicTarget ? gMusicTarget : gMusicVol + step;
    else if (gMusicVol > gMusicTarget) gMusicVol = gMusicVol - step < gMusicTarget ? gMusicTarget : gMusicVol - step;
    Mix_VolumeMusic((int)(gMusicVol * gMusicVol * MUSIC_MAX_VOLUME));
    if (gMusicTarget == 0 && gMusicVol <= 0 && !Mix_PausedMusic()) Mix_PauseMusic();
}

static void music_toggle(void)
{
    if (gMusicState < 0) return;
    if (gMusicState == 1) {
        gMusicState = 0;
        gMusicTarget = 0;
    } else {
        gMusicState = 1;
        gMusicTarget = 1;
        if (gMusic) Mix_ResumeMusic();
    }
}

static void music_shutdown(void)
{
    if (gMusic) {
        Mix_HaltMusic();
        Mix_FreeMusic(gMusic);
        gMusic = NULL;
    }
    Mix_CloseAudio();
    Mix_Quit();
}

/* ------------------------------------------------------------------ events + input */

static void poll_covers(void);
static void apply_search(const char *query);
static void batch_note(uint64_t done, uint64_t total, uint64_t bps);
static void chime(void);

/* Must run every frame: SDL's Wii U backend handles ProcUI (HOME menu, power-off, exit) in here. */
static void pump(void)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) {
            gQuit = true;
        } else if (ev.type == SDL_WINDOWEVENT) {
            if (ev.window.event == SDL_WINDOWEVENT_HIDDEN) gVisible = false;
            else if (ev.window.event == SDL_WINDOWEVENT_SHOWN || ev.window.event == SDL_WINDOWEVENT_EXPOSED)
                gVisible = true;
        } else if (ev.type == SDL_TEXTINPUT && gSearchOpen) {
            size_t have = strlen(gSearchBuf);
            snprintf(gSearchBuf + have, sizeof(gSearchBuf) - have, "%s", ev.text.text); /* may arrive in pieces */
        } else if (ev.type == SDL_SYSWMEVENT && ev.syswm.msg && gSearchOpen) {
            unsigned what = ev.syswm.msg->msg.wiiu.event;
            if (what == SDL_WIIU_SYSWM_SWKBD_OK_START_EVENT) {
                gSearchBuf[0] = '\0';
            } else if (what == SDL_WIIU_SYSWM_SWKBD_OK_FINISH_EVENT) {
                gSearchOpen = false;
                SDL_StopTextInput();
                apply_search(gSearchBuf);
            } else if (what == SDL_WIIU_SYSWM_SWKBD_CANCEL_EVENT) {
                gSearchOpen = false;
                SDL_StopTextInput();
            }
        }
    }
    /* safety net: the keyboard went away without telling us */
    if (gSearchOpen && now_s() - gSearchOpened > 1.0 && !SDL_IsScreenKeyboardShown(gWindow)) {
        gSearchOpen = false;
        SDL_StopTextInput();
    }
    music_update();
    poll_covers();
}

typedef struct {
    uint32_t pressed;
    bool tap;
    int tapX, tapY; /* canvas coordinates (1280x720) */
} Input;

/* Wii Remotes, Classic Controllers and Pro Controllers, translated to GamePad buttons. */
static void read_kpad(uint32_t *trigger, uint32_t *hold)
{
    static const struct { uint32_t from, to; } core[] = {
        {WPAD_BUTTON_UP, VPAD_BUTTON_UP},     {WPAD_BUTTON_DOWN, VPAD_BUTTON_DOWN},
        {WPAD_BUTTON_LEFT, VPAD_BUTTON_LEFT}, {WPAD_BUTTON_RIGHT, VPAD_BUTTON_RIGHT},
        {WPAD_BUTTON_A, VPAD_BUTTON_A},       {WPAD_BUTTON_B, VPAD_BUTTON_B},
        {WPAD_BUTTON_1, VPAD_BUTTON_X},       {WPAD_BUTTON_2, VPAD_BUTTON_Y},
        {WPAD_BUTTON_PLUS, VPAD_BUTTON_PLUS}, {WPAD_BUTTON_MINUS, VPAD_BUTTON_MINUS},
    };
    /* Classic and Pro use the same bits for these */
    static const struct { uint32_t from, to; } pro[] = {
        {WPAD_PRO_BUTTON_UP, VPAD_BUTTON_UP},     {WPAD_PRO_BUTTON_DOWN, VPAD_BUTTON_DOWN},
        {WPAD_PRO_BUTTON_LEFT, VPAD_BUTTON_LEFT}, {WPAD_PRO_BUTTON_RIGHT, VPAD_BUTTON_RIGHT},
        {WPAD_PRO_BUTTON_A, VPAD_BUTTON_A},       {WPAD_PRO_BUTTON_B, VPAD_BUTTON_B},
        {WPAD_PRO_BUTTON_X, VPAD_BUTTON_X},       {WPAD_PRO_BUTTON_Y, VPAD_BUTTON_Y},
        {WPAD_PRO_BUTTON_PLUS, VPAD_BUTTON_PLUS}, {WPAD_PRO_BUTTON_MINUS, VPAD_BUTTON_MINUS},
        {WPAD_PRO_BUTTON_L, VPAD_BUTTON_L},       {WPAD_PRO_BUTTON_R, VPAD_BUTTON_R},
        {WPAD_PRO_BUTTON_ZL, VPAD_BUTTON_ZL},     {WPAD_PRO_BUTTON_ZR, VPAD_BUTTON_ZR},
    };
    static uint32_t lastHold[4];
    for (int ch = 0; ch < 4; ch++) {
        KPADStatus k;
        KPADError ke = KPAD_ERROR_OK;
        memset(&k, 0, sizeof(k));
        uint32_t n = KPADReadEx((KPADChan)ch, &k, 1, &ke);
        if (n < 1 || ke != KPAD_ERROR_OK) {
            *hold |= lastHold[ch];
            continue;
        }
        if (gSearchOpen) SDL_WiiUSetSWKBDKPAD(ch, &k);
        uint32_t t = 0, h = 0;
        for (size_t i = 0; i < sizeof(core) / sizeof(core[0]); i++) {
            if (k.trigger & core[i].from) t |= core[i].to;
            if (k.hold & core[i].from) h |= core[i].to;
        }
        if (k.extensionType == WPAD_EXT_CLASSIC || k.extensionType == WPAD_EXT_MPLUS_CLASSIC ||
            k.extensionType == WPAD_EXT_PRO_CONTROLLER) {
            uint32_t et = k.extensionType == WPAD_EXT_PRO_CONTROLLER ? k.pro.trigger : k.classic.trigger;
            uint32_t eh = k.extensionType == WPAD_EXT_PRO_CONTROLLER ? k.pro.hold : k.classic.hold;
            for (size_t i = 0; i < sizeof(pro) / sizeof(pro[0]); i++) {
                if (et & pro[i].from) t |= pro[i].to;
                if (eh & pro[i].from) h |= pro[i].to;
            }
        }
        lastHold[ch] = h;
        *trigger |= t;
        *hold |= h;
    }
}

static Input read_input(void)
{
    static int heldFrames;
    static bool touching;
    static int startX, startY, lastX, lastY;
    Input in = {0};

    uint32_t kTrig = 0, kHold = 0;
    read_kpad(&kTrig, &kHold);

    /* read every sample queued since last time (up to 16) so short presses aren't missed when
       the caller only checks a few times a second (e.g. during downloads); newest is first */
    static VPADStatus buf[16];
    VPADReadError e;
    int got = VPADRead(VPAD_CHAN_0, buf, 16, &e);
    VPADStatus st;
    memset(&st, 0, sizeof(st));
    if (got >= 1 && e == VPAD_READ_SUCCESS) {
        st = buf[0];
        for (int k = 1; k < got; k++) st.trigger |= buf[k].trigger;
    }
    if (gSearchOpen) {
        /* the keyboard needs a calibrated touch point, or tapping its keys does nothing */
        if (st.tpNormal.touched) VPADGetTPCalibratedPoint(VPAD_CHAN_0, &st.tpNormal, &st.tpNormal);
        SDL_WiiUSetSWKBDVPAD(&st);
        heldFrames = 0;
        touching = false;
        return in;
    }

    in.pressed = st.trigger | kTrig;
    uint32_t dirs = (st.hold | kHold) & (VPAD_BUTTON_UP | VPAD_BUTTON_DOWN | VPAD_BUTTON_LEFT | VPAD_BUTTON_RIGHT);
    if (dirs) {
        heldFrames++;
        if (heldFrames > 18 && (heldFrames > 90 || heldFrames % 4 == 0)) in.pressed |= dirs; /* auto-repeat, faster when held */
    } else {
        heldFrames = 0;
    }

    /* touch: a short tap without much movement counts */
    if (st.tpNormal.touched && st.tpNormal.validity == VPAD_VALID) {
        VPADTouchData cal;
        VPADGetTPCalibratedPointEx(VPAD_CHAN_0, VPAD_TP_854X480, &cal, &st.tpNormal);
        int x = cal.x * UI_W / 854, y = cal.y * UI_H / 480;
        if (!touching) {
            startX = x;
            startY = y;
        }
        touching = true;
        lastX = x;
        lastY = y;
    } else if (touching) {
        touching = false;
        int dx = lastX - startX, dy = lastY - startY;
        if (dx * dx + dy * dy < 40 * 40) {
            in.tap = true;
            in.tapX = lastX;
            in.tapY = lastY;
        }
    }
    return in;
}

/* ------------------------------------------------------------------ files */

static void mkdir_p(const char *path)
{
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s", path);
    /* skip the drive itself ("fs:/vol/external01" or "usb:") so we don't try to create it */
    size_t skip = !strncmp(tmp, SD_ROOT, strlen(SD_ROOT)) ? strlen(SD_ROOT) : strchr(tmp, ':') ? (size_t)(strchr(tmp, ':') - tmp + 1) : 0;
    for (char *p = tmp + skip + 1; *p && p > tmp; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
    mkdir(tmp, 0777);
}

static bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static uint64_t sd_free_bytes(void)
{
    struct statvfs v;
    if (statvfs(gUseUsb ? USB_ROOT "/" : SD_ROOT "/", &v) != 0) return 0; /* unknown */
    return (uint64_t)v.f_bavail * (uint64_t)v.f_frsize;
}

static void update_sd_text(void)
{
    uint64_t f = sd_free_bytes();
    if (!f) {
        gSdText[0] = '\0';
        return;
    }
    char s[24];
    fmt_size(s, sizeof(s), f);
    snprintf(gSdText, sizeof(gSdText), "%s%s free", gUseUsb ? "USB: " : "", s);
}

/* FNV-1a, used to give each link its own staging folder and installed.txt key. */
static void url_hash(const char *url, char out[9])
{
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)url; *p; p++) {
        h ^= *p;
        h *= 16777619u;
    }
    snprintf(out, 9, "%08" PRIx32, h);
}

/* ------------------------------------------------------------------ entries */

typedef struct {
    Link link;
    char hash[9];
    char subtitle[400];
    char display[128];   /* shown name: user's name, else the disc title once known, else file name */
    char discId[7];      /* ID from installed.txt, the ID cache, the file name or a header peek */
    char discTitle[65];
    SDL_Texture *cover;  /* loaded only while the entry is near the selection */
    int coverState;      /* COVER_UNKNOWN / COVER_CACHED / COVER_NONE */
    /* filled once installed (from installed.txt) */
    char id[7];
    char folder[96];
    int disc; /* GameCube disc 2 -> disc2.iso next to game.iso */
    bool usb; /* installed on the USB drive */
    bool gc;
    bool installed;
    bool partial;
    bool fav;
    int metacritic;      /* from RAWG: 1-100, 0 = none */
    uint64_t remoteSize; /* from a HEAD request on the details screen (0 = unknown) */
    char coverWhy[120];  /* why the cover worker found no ID / cover (details screen) */
} Entry;

static Entry *entries;
static int entryCount;
static UiItem *items;

/* download queue: entry indices in order */
static int *gQueue;
static int gQueueCount;

static int queue_pos(int idx) /* 1-based, 0 = not queued */
{
    for (int i = 0; i < gQueueCount; i++)
        if (gQueue[i] == idx) return i + 1;
    return 0;
}

static void make_folder_name(const char *title, const char *id, char *out, size_t n)
{
    char clean[64];
    int j = 0;
    for (const char *s = title; *s && j < (int)sizeof(clean) - 1; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || strchr("\\/:*?\"<>|", c)) continue;
        clean[j++] = (char)c;
    }
    while (j > 0 && (clean[j - 1] == ' ' || clean[j - 1] == '.')) j--;
    clean[j] = '\0';
    if (!clean[0]) snprintf(clean, sizeof(clean), "%s", id);
    snprintf(out, n, "%s [%s]", clean, id);
}

/* The raw download: a .wbfs, a GameCube .iso or a .7z, split for FAT32 as dl.000, dl.001, ... */
static void staging_writer(const char *hash, SplitWriter *sw)
{
    memset(sw, 0, sizeof(*sw));
    snprintf(sw->base, sizeof(sw->base), "%s/%s/dl", gTmpDir, hash);
    sw->ext0 = ".000";
    sw->partfmt = ".%03d";
    sw->split = 1;
    sw->part_size = WBFS_SPLIT_SIZE;
}

/* The game pulled out of a .7z, already split the way USB Loader GX wants. */
static void extract_writer(const char *hash, SplitWriter *sw)
{
    memset(sw, 0, sizeof(*sw));
    snprintf(sw->base, sizeof(sw->base), "%s/%s/game", gTmpDir, hash);
    sw->ext0 = ".wbfs";
    sw->split = 1;
    sw->part_size = WBFS_SPLIT_SIZE;
}

typedef enum { STAGED_BAD = 0, STAGED_DISC, STAGED_7Z, STAGED_RVZ } StagedKind;

static StagedKind staged_kind(const SplitWriter *sw)
{
    char p0[600];
    uint8_t buf[DISC_HEADER_NEED];
    sw_part_path(sw, 0, p0, sizeof(p0));
    FILE *f = fopen(p0, "rb");
    if (!f) return STAGED_BAD;
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (sz_is_7z(buf, n)) return STAGED_7Z;
    if (rvz_is_rvz(buf, n)) return STAGED_RVZ;
    DiscInfo info;
    DiscKind k = disc_identify(buf, n, &info);
    return (k == DISC_WII_WBFS || k == DISC_GC_ISO) ? STAGED_DISC : STAGED_BAD;
}

static void final_writer(bool usb, bool gc, int disc, const char *folder, const char *id, SplitWriter *sw)
{
    memset(sw, 0, sizeof(*sw));
    if (gc) {
        /* Nintendont: disc 1 is game.iso, disc 2 is disc2.iso in the same folder */
        snprintf(sw->base, sizeof(sw->base), "%s/games/%s/%s", game_root(usb), folder, disc ? "disc2" : "game");
        sw->ext0 = ".iso";
        sw->split = 0;
        sw->part_size = UINT64_MAX;
    } else {
        snprintf(sw->base, sizeof(sw->base), "%s/wbfs/%s/%s", game_root(usb), folder, id);
        sw->ext0 = ".wbfs";
        sw->split = 1;
        sw->part_size = WBFS_SPLIT_SIZE;
    }
}

/* installed.txt: hash <TAB> wii|gc <TAB> ID <TAB> folder */
static void load_installed(void)
{
    FILE *f = fopen(INSTALLED_PATH, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *f0 = strtok(line, "\t"), *f1 = strtok(NULL, "\t"), *f2 = strtok(NULL, "\t"),
             *f3 = strtok(NULL, "\t"), *f4 = strtok(NULL, "\t"), *f5 = strtok(NULL, "\t");
        if (!f0 || !f1 || !f2 || !f3) continue;
        for (int i = 0; i < entryCount; i++) {
            Entry *e = &entries[i];
            if (strcmp(e->hash, f0)) continue;
            e->gc = !strcmp(f1, "gc");
            snprintf(e->id, sizeof(e->id), "%s", f2);
            snprintf(e->folder, sizeof(e->folder), "%s", f3);
            e->disc = ((f4 && !strcmp(f4, "2")) || (f5 && !strcmp(f5, "2"))) ? 1 : 0;
            e->usb = (f4 && !strcmp(f4, "usb")) || (f5 && !strcmp(f5, "usb"));
        }
    }
    fclose(f);
}

static void save_installed(void)
{
    FILE *f = fopen(INSTALLED_PATH, "w");
    if (!f) return;
    for (int i = 0; i < entryCount; i++) {
        Entry *e = &entries[i];
        if (e->folder[0])
            fprintf(f, "%s\t%s\t%s\t%s%s%s\n", e->hash, e->gc ? "gc" : "wii", e->id, e->folder, e->disc ? "\t2" : "",
                    e->usb ? "\tusb" : "");
    }
    fclose(f);
}

/* "https://user:pw@host:8080/a/My%20Game.7z?dl=1" -> host "host:8080", file "My Game.7z" */
static void url_parts(const char *url, char *host, size_t hn, char *file, size_t fn)
{
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    const char *hostEnd = p + strcspn(p, "/?#");
    const char *at = memchr(p, '@', hostEnd - p);
    if (at) p = at + 1;
    snprintf(host, hn, "%.*s", (int)(hostEnd - p), p);

    const char *end = url + strcspn(url, "?#");
    const char *start = end;
    while (start > hostEnd && start[-1] != '/') start--;
    size_t j = 0;
    for (const char *q = start; q < end && j + 1 < fn; q++) {
        if (*q == '%' && q + 2 < end) {
            char hex[3] = {q[1], q[2], 0};
            char *endp;
            long v = strtol(hex, &endp, 16);
            if (*endp == '\0') {
                file[j++] = (char)v;
                q += 2;
                continue;
            }
        }
        file[j++] = *q == '+' ? ' ' : *q;
    }
    file[j] = '\0';
}

static bool ends_with_ci(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && !strcasecmp(s + a - b, suf);
}

/* Startup speed for big lists: list the temp folder once, so links that were never started need no
   SD-card checks at all (the per-link checks were ~5 file lookups x thousands of links). */
static char (*gTmpSet)[9];
static int gTmpCount;
static bool gTmpSetValid;

static int cmp_hash(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

static void scan_tmp_dir(void)
{
    free(gTmpSet);
    gTmpSet = NULL;
    gTmpCount = 0;
    int cap = 0;
    DIR *d = opendir(gTmpDir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (strlen(de->d_name) != 8) continue;
            if (gTmpCount == cap) {
                cap = cap ? cap * 2 : 64;
                char(*n)[9] = realloc(gTmpSet, (size_t)cap * 9);
                if (!n) break;
                gTmpSet = n;
            }
            snprintf(gTmpSet[gTmpCount++], 9, "%s", de->d_name);
        }
        closedir(d);
    }
    if (gTmpCount) qsort(gTmpSet, (size_t)gTmpCount, 9, cmp_hash);
    gTmpSetValid = true;
}

static bool tmp_has(const char *hash)
{
    return gTmpCount && bsearch(hash, gTmpSet, (size_t)gTmpCount, 9, cmp_hash) != NULL;
}

static void refresh_status(Entry *e)
{
    SplitWriter sw;
    char path[600];
    e->installed = false;
    if (e->folder[0]) {
        final_writer(e->usb, e->gc, e->disc, e->folder, e->id, &sw);
        sw_part_path(&sw, 0, path, sizeof(path));
        e->installed = file_exists(path);
    }
    e->partial = false;
    char statePath[400];
    if (gTmpSetValid && !tmp_has(e->hash)) goto subtitle; /* never started: nothing in tmp */
    staging_writer(e->hash, &sw);
    e->partial = sw_existing_size(&sw) > 0;
    snprintf(statePath, sizeof(statePath), "%s/%s/convert.state", gTmpDir, e->hash);
    if (!e->partial && wc_has_state(statePath)) e->partial = true;
    snprintf(statePath, sizeof(statePath), "%s/%s/zip.ckpt", gTmpDir, e->hash);
    if (!e->partial && file_exists(statePath)) e->partial = true;

subtitle:;
    char host[128], file[200];
    url_parts(e->link.url, host, sizeof(host), file, sizeof(file));
    if (e->installed)
        snprintf(e->subtitle, sizeof(e->subtitle), "%s  -  on SD in /%s", e->id, e->gc ? "games" : "wbfs");
    else if (e->partial)
        snprintf(e->subtitle, sizeof(e->subtitle), "%s  -  partly downloaded, A to resume", host);
    else
        snprintf(e->subtitle, sizeof(e->subtitle), "%s  -  %s", host, file);
}

static bool gViewDirty = true;

static void fill_item(int i)
{
    gViewDirty = true;
    {
        Entry *e = &entries[i];
        UiItem *it = &items[i];
        it->title = e->display[0] ? e->display : e->link.name;
        it->subtitle = e->subtitle;
        it->cover = e->cover;
        it->fav = e->fav;
        it->queuePos = queue_pos(i);
        it->metacritic = e->metacritic;
        it->status = e->installed ? UI_ST_INSTALLED : e->partial ? UI_ST_PARTIAL : UI_ST_NEW;
        if (e->installed) it->kind = e->gc ? UI_KIND_GC : UI_KIND_WII;
        else if (e->discId[0] && !ends_with_ci(e->link.url, ".7z") && !ends_with_ci(e->link.url, ".zip"))
            it->kind = (e->discId[0] == 'G' || e->discId[0] == 'D') ? UI_KIND_GC : UI_KIND_WII;
        else {
            char host[8], file[200];
            url_parts(e->link.url, host, sizeof(host), file, sizeof(file));
            it->kind = ends_with_ci(file, ".7z")     ? UI_KIND_7Z
                       : ends_with_ci(file, ".zip")  ? UI_KIND_ZIP
                       : ends_with_ci(file, ".wbfs") ? UI_KIND_WII
                       : ends_with_ci(file, ".iso")  ? UI_KIND_GC
                                                     : UI_KIND_UNKNOWN;
        }
    }
}

static void rebuild_items(void)
{
    gViewDirty = true;
    free(items);
    items = entryCount ? calloc((size_t)entryCount, sizeof(UiItem)) : NULL;
    for (int i = 0; i < entryCount && items; i++) fill_item(i);
}

/* ---- the visible list (search filter) ---- */

static int *gView, *gPosOf;
static int gViewCount;
static UiItem *gViewItems;

static void lower_copy(char *dst, size_t n, const char *src)
{
    size_t j = 0;
    for (; *src && j + 1 < n; src++) dst[j++] = (char)((*src >= 'A' && *src <= 'Z') ? *src + 32 : *src);
    dst[j] = '\0';
}

static const char *entry_title(int i)
{
    return items && items[i].title ? items[i].title : entries[i].link.name;
}

static int cmp_queue(const void *a, const void *b)
{
    return queue_pos(*(const int *)a) - queue_pos(*(const int *)b);
}

static int cmp_view(const void *a, const void *b)
{
    int ia = *(const int *)a, ib = *(const int *)b;
    if (gSort == SORT_INSTALLED && entries[ia].installed != entries[ib].installed) return entries[ia].installed ? -1 : 1;
    if (gSort == SORT_META && entries[ia].metacritic != entries[ib].metacritic)
        return entries[ib].metacritic - entries[ia].metacritic; /* best first, no score last */
    if (gSort == SORT_AZ) {
        int c = strcasecmp(entry_title(ia), entry_title(ib));
        if (c) return c;
    }
    return ia - ib; /* list order */
}

/* Region from a game ID ("RMCP01" -> P) or Redump-style tags in a name ("(Europe)" -> P). */
enum { RG_USA = 1, RG_EUR = 2, RG_JPN = 4, RG_KOR = 8, RG_ALL = 15 };

static int region_bit(char letter)
{
    return letter == 'E' ? RG_USA : letter == 'P' ? RG_EUR : letter == 'J' ? RG_JPN : letter == 'K' ? RG_KOR : 0;
}

/* Which regions a game is for: from Redump / No-Intro tags in the name ("(USA, Europe)", "(World)"), else
   from the game ID's 4th letter (RMCE01 -> USA, RMCP01 -> Europe). 0 = can't tell. */
static int entry_regions(const Entry *e)
{
    static const struct { const char *word; int bits; } words[] = {
        {"usa", RG_USA},         {"canada", RG_USA},     {"ntsc-u", RG_USA},    {"europe", RG_EUR},
        {"uk", RG_EUR},          {"germany", RG_EUR},    {"france", RG_EUR},    {"spain", RG_EUR},
        {"italy", RG_EUR},       {"netherlands", RG_EUR}, {"sweden", RG_EUR},   {"scandinavia", RG_EUR},
        {"australia", RG_EUR},   {"pal", RG_EUR},        {"japan", RG_JPN},     {"ntsc-j", RG_JPN},
        {"korea", RG_KOR},       {"world", RG_ALL},
    };
    int bits = 0;
    const char *n = e->link.name;
    for (const char *open = strchr(n, '('); open; open = strchr(open + 1, '(')) {
        const char *close = strchr(open, ')');
        if (!close) break;
        /* words inside this (...) group, separated by commas / spaces */
        for (const char *w = open + 1; w < close;) {
            while (w < close && (*w == ',' || *w == ' ')) w++;
            const char *we = w;
            while (we < close && *we != ',' && *we != ' ') we++;
            size_t len = (size_t)(we - w);
            for (size_t k = 0; k < sizeof(words) / sizeof(words[0]); k++)
                if (len == strlen(words[k].word) && !strncasecmp(w, words[k].word, len)) bits |= words[k].bits;
            w = we;
        }
    }
    if (bits) return bits;
    if (e->discId[0]) {
        char r = e->discId[3];
        if (strchr("PDFSIXYUH", r)) return RG_EUR;
        return region_bit(r);
    }
    return 0;
}

static void regions_text(int bits, char *out, size_t n)
{
    if (!bits) {
        snprintf(out, n, "Unknown");
        return;
    }
    if ((bits & RG_ALL) == RG_ALL) {
        snprintf(out, n, "World");
        return;
    }
    out[0] = '\0';
    static const char *names[] = {"USA", "Europe", "Japan", "Korea"};
    for (int i = 0; i < 4; i++)
        if (bits & (1 << i)) snprintf(out + strlen(out), n - strlen(out), "%s%s", out[0] ? ", " : "", names[i]);
}

/* "Mario Kart Wii (USA) (En,Fr).zip" and "Mario Kart Wii" -> "mariokartwii" */
static void group_key(int i, char *out, size_t n)
{
    const char *t = entries[i].link.name; /* (not the disc title: regions often spell it differently) */
    char base[256];
    snprintf(base, sizeof(base), "%s", t);
    static const char *exts[] = {".zip", ".7z", ".wbfs", ".iso", ".rvz", ".gcm", ".wia"};
    for (int k = 0; k < 2; k++)
        for (size_t x = 0; x < sizeof(exts) / sizeof(exts[0]); x++)
            if (ends_with_ci(base, exts[x])) base[strlen(base) - strlen(exts[x])] = '\0';
    size_t j = 0;
    int depth = 0;
    for (const char *s = base; *s && j + 1 < n; s++) {
        char c = *s;
        if (c == '(' || c == '[') depth++;
        else if ((c == ')' || c == ']') && depth > 0) depth--;
        else if (!depth) {
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out[j++] = c;
        }
    }
    out[j] = '\0';
    if (!j) snprintf(out, n, "#%s", entries[i].hash); /* nothing to compare: never a duplicate */
    /* "(Disc 2)" is part of the game, not a region tag: disc 1 and disc 2 are never duplicates */
    for (const char *s = base; *s; s++)
        if (!strncasecmp(s, "(disc ", 6) && s[6] >= '0' && s[6] <= '9' && j + 6 < n) {
            snprintf(out + j, n - j, "#disc%c", s[6]);
            break;
        }
}

static char (*gKeys)[64];
static int cmp_key_idx(const void *a, const void *b)
{
    int ia = *(const int *)a, ib = *(const int *)b;
    int c = strcmp(gKeys[ia], gKeys[ib]);
    return c ? c : ia - ib;
}

/* Keep one entry per game: the installed copy, else your preferred region, else the first listed. */
static void hide_duplicates(void)
{
    if (gViewCount < 2) return;
    gKeys = malloc((size_t)entryCount * 64);
    int *order = malloc((size_t)gViewCount * sizeof(int));
    bool *keep = calloc((size_t)entryCount, sizeof(bool));
    if (!gKeys || !order || !keep) goto done;
    for (int p = 0; p < gViewCount; p++) {
        group_key(gView[p], gKeys[gView[p]], 64);
        order[p] = gView[p];
    }
    qsort(order, (size_t)gViewCount, sizeof(int), cmp_key_idx);
    for (int a = 0; a < gViewCount;) {
        int b = a, best = order[a], bestScore = -1;
        while (b < gViewCount && !strcmp(gKeys[order[b]], gKeys[order[a]])) {
            Entry *e = &entries[order[b]];
            int score = (e->installed ? 4 : 0) + (e->partial ? 2 : 0) + ((entry_regions(e) & region_bit(gRegionPref)) ? 1 : 0);
            if (score > bestScore) { /* ties: the first one listed (order is by index within a group) */
                bestScore = score;
                best = order[b];
            }
            b++;
        }
        keep[best] = true;
        a = b;
    }
    int n = 0;
    for (int p = 0; p < gViewCount; p++)
        if (keep[gView[p]]) gView[n++] = gView[p];
    gViewCount = n;
done:
    free(gKeys);
    gKeys = NULL;
    free(order);
    free(keep);
}

/* Every word of the query must appear in the title (or the game ID / file name): "mario usa" works. */
static void build_view(void)
{
    free(gView);
    free(gPosOf);
    gView = entryCount ? malloc((size_t)entryCount * sizeof(int)) : NULL;
    gPosOf = entryCount ? malloc((size_t)entryCount * sizeof(int)) : NULL;
    gViewCount = 0;
    gViewDirty = true;
    if (!gView || !gPosOf) return;

    char q[128], *words[16];
    int nw = 0;
    lower_copy(q, sizeof(q), gQuery);
    for (char *t = strtok(q, " \t"); t && nw < 16; t = strtok(NULL, " \t")) words[nw++] = t;

    char hay[512];
    for (int i = 0; i < entryCount; i++) {
        gPosOf[i] = -1;
        Entry *e = &entries[i];
        bool ok = gShow == SHOW_ALL || (gShow == SHOW_FAV && e->fav) || (gShow == SHOW_INSTALLED && e->installed) ||
                  (gShow == SHOW_NOT_INSTALLED && !e->installed) || (gShow == SHOW_QUEUED && queue_pos(i));
        if (ok && gRegionOnly) { /* games whose region can't be told stay visible */
            int rb = entry_regions(e);
            if (rb && !(rb & region_bit(gRegionOnly))) ok = false;
        }
        if (ok && nw > 0) {
            char raw[512];
            snprintf(raw, sizeof(raw), "%s %s %s", items ? items[i].title : e->link.name, e->discId, e->link.name);
            lower_copy(hay, sizeof(hay), raw);
            for (int w = 0; w < nw && ok; w++) ok = strstr(hay, words[w]) != NULL;
        }
        if (ok) gView[gViewCount++] = i;
    }
    if (gHideDupes) hide_duplicates();
    if (gShow == SHOW_QUEUED && gViewCount > 1) qsort(gView, (size_t)gViewCount, sizeof(int), cmp_queue);
    else if (gSort != SORT_LIST && gViewCount > 1) qsort(gView, (size_t)gViewCount, sizeof(int), cmp_view);
    for (int p = 0; p < gViewCount; p++) gPosOf[gView[p]] = p;
}

static const UiItem *view_items(void)
{
    if (gViewDirty) {
        free(gViewItems);
        gViewItems = gViewCount ? malloc((size_t)gViewCount * sizeof(UiItem)) : NULL;
        for (int p = 0; p < gViewCount && gViewItems && items; p++) gViewItems[p] = items[gView[p]];
        gViewDirty = false;
    }
    return gViewItems;
}

static int cur_entry(void)
{
    return (gViewCount > 0 && gSel >= 0 && gSel < gViewCount) ? gView[gSel] : -1;
}

static void apply_search(const char *query)
{
    int keep = cur_entry();
    char q[128];
    snprintf(q, sizeof(q), "%s", query ? query : "");
    char *a = q, *b = q + strlen(q);
    while (*a == ' ') a++;
    while (b > a && b[-1] == ' ') *--b = '\0';
    snprintf(gQuery, sizeof(gQuery), "%s", a);
    build_view();
    gSel = (keep >= 0 && gPosOf && gPosOf[keep] >= 0) ? gPosOf[keep] : 0;
}

static void open_search(void)
{
    SDL_WiiUSetSWKBDOKLabel("Search");
    SDL_WiiUSetSWKBDHintText("Game name, e.g. mario kart");
    SDL_WiiUSetSWKBDInitialText(gQuery);
    SDL_WiiUSetSWKBDHighlightInitialText(SDL_TRUE);
    SDL_WiiUSetSWKBDShowWordSuggestions(SDL_FALSE);
    gSearchBuf[0] = '\0';
    gSearchOpen = true;
    gSearchOpened = now_s();
    SDL_StartTextInput();
}

/* ---- favorites, queue and list options survive restarts ---- */

static int find_entry(const char *hash)
{
    for (int i = 0; i < entryCount; i++)
        if (!strcmp(entries[i].hash, hash)) return i;
    return -1;
}

static void save_hash_list(const char *path, bool favs)
{
    FILE *f = fopen(path, "w");
    if (!f) return;
    if (favs) {
        for (int i = 0; i < entryCount; i++)
            if (entries[i].fav) fprintf(f, "%s\n", entries[i].hash);
    } else {
        for (int i = 0; i < gQueueCount; i++) fprintf(f, "%s\n", entries[gQueue[i]].hash);
    }
    fclose(f);
}

static void load_lists(void)
{
    free(gQueue);
    gQueue = entryCount ? malloc((size_t)entryCount * sizeof(int)) : NULL;
    gQueueCount = 0;
    char line[64];
    FILE *f = fopen(FAV_PATH, "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = '\0';
            int i = find_entry(line);
            if (i >= 0) entries[i].fav = true;
        }
        fclose(f);
    }
    f = gQueue ? fopen(QUEUE_PATH, "r") : NULL;
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = '\0';
            int i = find_entry(line);
            if (i >= 0 && !queue_pos(i)) gQueue[gQueueCount++] = i;
        }
        fclose(f);
    }
}

static void queue_toggle(int idx)
{
    if (!gQueue) return;
    int pos = queue_pos(idx);
    if (pos) {
        memmove(gQueue + pos - 1, gQueue + pos, (size_t)(gQueueCount - pos) * sizeof(int));
        gQueueCount--;
    } else {
        gQueue[gQueueCount++] = idx;
    }
    save_hash_list(QUEUE_PATH, false);
}

static void save_state(void)
{
    FILE *f = fopen(STATE_PATH, "w");
    if (!f) return;
    fprintf(f, "sort=%d\nshow=%d\ndupes=%d\nregion=%c\nonly=%c\nmusic=%d\nqueueoff=%d\nchime=%d\n", gSort, gShow,
            gHideDupes ? 1 : 0, gRegionPref, gRegionOnly ? gRegionOnly : 'A', gMusicState == 1 ? 1 : 0, gQueueOff ? 1 : 0,
            gChimeOn ? 1 : 0);
    fprintf(f, "target=%s\nusbseen=%d\n", gUseUsb ? "usb" : "sd", gUsbSeen ? 1 : 0);
    fclose(f);
}

static void load_state(int *music)
{
    gRegionPref = !strncasecmp(gCfg.region, "eu", 2) || !strncasecmp(gCfg.region, "pal", 3) ? 'P'
                  : !strncasecmp(gCfg.region, "ja", 2)                                        ? 'J'
                                                                                              : 'E';
    FILE *f = fopen(STATE_PATH, "r");
    if (!f) return;
    char line[64];
    while (fgets(line, sizeof(line), f)) {
        int v;
        char c;
        if (sscanf(line, "sort=%d", &v) == 1 && v >= 0 && v < SORT_COUNT) gSort = v;
        else if (sscanf(line, "show=%d", &v) == 1 && v >= 0 && v < SHOW_COUNT) gShow = v;
        else if (sscanf(line, "dupes=%d", &v) == 1) gHideDupes = v != 0;
        else if (sscanf(line, "region=%c", &c) == 1 && strchr("EPJ", c)) gRegionPref = c;
        else if (sscanf(line, "only=%c", &c) == 1) gRegionOnly = strchr("EPJ", c) ? c : 0;
        else if (sscanf(line, "queueoff=%d", &v) == 1) gQueueOff = v != 0;
        else if (sscanf(line, "chime=%d", &v) == 1) gChimeOn = v != 0;
        else if (!strncmp(line, "target=usb", 10)) gUseUsb = true;
        else if (sscanf(line, "usbseen=%d", &v) == 1) gUsbSeen = v != 0;
        else if (sscanf(line, "music=%d", &v) == 1) *music = v != 0;
    }
    fclose(f);
}

static void refresh_entry(Entry *e)
{
    refresh_status(e);
    rebuild_items();
}

/* ------------------------------------------------------------------ cover art */

static bool valid_id(const char *s)
{
    for (int i = 0; i < 6; i++)
        if (!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9'))) return false;
    return true;
}

/* "RMCE01.wbfs", "Mario Kart [RMCE01].7z" -> RMCE01 */
static bool id_from_filename(const char *file, char out[7])
{
    const char *b = strchr(file, '[');
    while (b) {
        if (strlen(b) >= 8 && b[7] == ']' && valid_id(b + 1)) {
            snprintf(out, 7, "%.6s", b + 1);
            return true;
        }
        b = strchr(b + 1, '[');
    }
    const char *dot = strrchr(file, '.');
    if (dot && dot - file == 6 && valid_id(file)) {
        snprintf(out, 7, "%.6s", file);
        return true;
    }
    return false;
}

static void set_display(Entry *e)
{
    const char *src = (!e->link.named && e->discTitle[0]) ? e->discTitle : e->link.name;
    size_t n = strlen(src);
    if (n >= sizeof(e->display)) n = sizeof(e->display) - 1;
    memmove(e->display, src, n);
    e->display[n] = 0;
}

enum { COVER_UNKNOWN = 0, COVER_CACHED, COVER_NONE };

static void load_cover(Entry *e)
{
    if (e->cover || !e->discId[0] || !covers_have(e->discId)) return;
    char path[300];
    covers_path(e->discId, path, sizeof(path));
    SDL_Surface *surf = IMG_Load(path);
    if (!surf) return;
    e->cover = SDL_CreateTextureFromSurface(gRenderer, surf);
    SDL_FreeSurface(surf);
}

#define MAX_LOADED_COVERS 96
static int gLoaded[MAX_LOADED_COVERS]; /* entries whose cover texture is loaded */
static int gLoadedCount;
static int gWinCenter = -100000;       /* selection when covers were last requested */

static void free_covers(void)
{
    ui_forget_covers();
    for (int i = 0; i < entryCount; i++) {
        if (entries[i].cover) SDL_DestroyTexture(entries[i].cover);
        entries[i].cover = NULL;
        if (items) items[i].cover = NULL;
    }
    gLoadedCount = 0;
    gWinCenter = -100000;
}

static void drop_cover(int k)
{
    int i = gLoaded[k];
    if (i < entryCount && entries[i].cover) {
        SDL_DestroyTexture(entries[i].cover);
        entries[i].cover = NULL;
        if (items) items[i].cover = NULL;
        gViewDirty = true;
    }
    gLoaded[k] = gLoaded[--gLoadedCount];
}

/* how far an entry is from the selection in the visible list (hidden entries count as far away) */
static int view_dist(int entry)
{
    int pos = (gPosOf && entry < entryCount) ? gPosOf[entry] : -1;
    return pos < 0 ? 1 << 30 : abs(pos - gSel);
}

/* How the cover worker can read the game ID off the server: the file's first bytes, the start of the
   game inside a .zip, or not at all for a .7z (then it goes by the file name). */
static int peek_mode(const Entry *e)
{
    char host[8], file[200];
    url_parts(e->link.url, host, sizeof(host), file, sizeof(file));
    if (ends_with_ci(file, ".7z")) return PEEK_NONE;
    if (ends_with_ci(file, ".zip")) return PEEK_ZIP;
    return PEEK_RAW;
}

/* Every frame: load covers for games near the selection (2 per frame keeps scrolling smooth), free
   the ones far away, and ask the worker only about links around what's on screen. */
static void cover_window_update(void)
{
    if (!entryCount || !items || !gView) return;
    int loadedNow = 0;
    for (int d = 0; d <= 10 && loadedNow < 2; d++) {
        for (int sgn = 1; sgn >= -1 && loadedNow < 2; sgn -= 2) {
            int pos = gSel + d * sgn;
            if ((d == 0 && sgn < 0) || pos < 0 || pos >= gViewCount) continue;
            int i = gView[pos];
            Entry *e = &entries[i];
            if (e->cover || e->coverState != COVER_CACHED) continue;
            while (gLoadedCount >= MAX_LOADED_COVERS) { /* free the one farthest away */
                int far = 0;
                for (int k = 1; k < gLoadedCount; k++)
                    if (view_dist(gLoaded[k]) > view_dist(gLoaded[far])) far = k;
                drop_cover(far);
            }
            load_cover(e);
            if (e->cover) {
                gLoaded[gLoadedCount++] = i;
                items[i].cover = e->cover;
                gViewDirty = true;
            } else {
                e->coverState = COVER_NONE;
            }
            loadedNow++;
        }
    }
    for (int k = 0; k < gLoadedCount;) {
        if (view_dist(gLoaded[k]) > 40) drop_cover(k);
        else k++;
    }

    if (abs(gSel - gWinCenter) < 4) return;
    gWinCenter = gSel;
    CoverJob *jobs = calloc(40, sizeof(CoverJob));
    if (!jobs) return;
    int n = 0;
    for (int d = 0; d <= 24 && n < 40; d++) {
        for (int sgn = 1; sgn >= -1 && n < 40; sgn -= 2) {
            int pos = gSel + d * sgn;
            if ((d == 0 && sgn < 0) || pos < 0 || pos >= gViewCount || (sgn < 0 && d > 8)) continue;
            int i = gView[pos];
            Entry *e = &entries[i];
            if (e->coverState != COVER_UNKNOWN) continue;
            CoverJob *j = &jobs[n++];
            j->index = i;
            snprintf(j->hash, sizeof(j->hash), "%s", e->hash);
            snprintf(j->url, sizeof(j->url), "%s", e->link.url);
            snprintf(j->id, sizeof(j->id), "%s", e->discId);
            j->peek = peek_mode(e);
            char host[8];
            url_parts(e->link.url, host, sizeof(host), j->name, sizeof(j->name));
            if (e->link.named) snprintf(j->altName, sizeof(j->altName), "%s", e->link.name);
        }
    }
    covers_request(jobs, n);
    free(jobs);
}

/* Work out what we already know about one link (no SD access: ID cache + cover folder scan). */
static void prepare_cover(Entry *e)
{
    char host[8], file[200], title[65] = "";
    url_parts(e->link.url, host, sizeof(host), file, sizeof(file));
    if (e->id[0]) snprintf(e->discId, sizeof(e->discId), "%s", e->id);
    else if (covers_cached_id(e->hash, e->discId, title)) snprintf(e->discTitle, sizeof(e->discTitle), "%s", title);
    else id_from_filename(file, e->discId);
    set_display(e);
    e->coverState = covers_have(e->discId) ? COVER_CACHED : COVER_UNKNOWN;
    int m = 0, u = 0;
    covers_score(e->discId, &m, &u);
    e->metacritic = m;
}

/* Pick up whatever the worker finished since last frame. */
static bool gRetryCovers;
static double gRetryAt;

static void poll_covers(void)
{
    CoverJob done[8];
    int n;
    bool changed = false;
    while ((n = covers_poll(done, 8)) > 0) {
        for (int k = 0; k < n; k++) {
            int i = done[k].index;
            if (i < 0 || i >= entryCount || strcmp(entries[i].hash, done[k].hash)) continue; /* list reloaded */
            Entry *e = &entries[i];
            if (!e->discId[0] && done[k].id[0]) snprintf(e->discId, sizeof(e->discId), "%s", done[k].id);
            if (!e->discTitle[0] && done[k].title[0]) snprintf(e->discTitle, sizeof(e->discTitle), "%s", done[k].title);
            set_display(e);
            snprintf(e->coverWhy, sizeof(e->coverWhy), "%s", done[k].why);
            if (!e->metacritic) { /* the ID may be new: look it up in the built-in table */
                int m = 0, u = 0;
                covers_score(e->discId, &m, &u);
                e->metacritic = m;
            }
            /* texture loads when near; a temporary failure (network, title list) is asked again later */
            e->coverState = done[k].hasCover ? COVER_CACHED : (done[k].retry && !e->discId[0]) ? COVER_UNKNOWN : COVER_NONE;
            if (done[k].retry) gRetryCovers = true;
            if (items) fill_item(i);
            changed = true;
        }
    }
    (void)changed;
    if (gRetryCovers) {
        gRetryCovers = false;
        gRetryAt = now_s() + 60;
    } else if (gRetryAt > 0 && now_s() > gRetryAt) {
        gRetryAt = 0;
        gWinCenter = -100000; /* re-request the covers around the selection */
    }
}

static void show_progress(const UiProgress *p);

static void show_loading(int done, int total)
{
    UiProgress p = {0};
    p.stages[0] = "Loading";
    p.stageCount = 1;
    p.title = "Loading your game list";
    char detail[64];
    snprintf(detail, sizeof(detail), total ? "%d of %d titles" : "Reading links.txt...", done, total);
    p.detail = detail;
    p.done = (uint64_t)done;
    p.total = (uint64_t)total;
    pump();
    show_progress(&p);
}

/* Returns entry count, or -1 if links.txt was missing (template written). */
static int load_entries(void)
{
    show_loading(0, 0);
    free_covers();
    free(items); /* cover results that arrive while loading must not touch the old list */
    items = NULL;
    free(entries);
    entries = NULL;
    entryCount = 0;

    /* links_url: the list lives on your server; the last copy that loaded is kept for when it can't be reached */
    const char *listPath = LINKS_PATH;
    gListNote[0] = '\0';
    if (gCfg.links_url[0]) {
        char err[256] = {0};
        char *txt = net_fetch_text(&gCfg, gCfg.links_url, err, sizeof(err));
        if (txt && strstr(txt, "http")) {
            FILE *f = fopen(REMOTE_LINKS, "w");
            if (f) {
                fputs(txt, f);
                fclose(f);
            }
            listPath = REMOTE_LINKS;
        } else {
            if (txt) snprintf(err, sizeof(err), "it has no links in it");
            bool cached = file_exists(REMOTE_LINKS);
            if (cached) listPath = REMOTE_LINKS;
            snprintf(gListNote, sizeof(gListNote), "Couldn't load your list from links_url (%s). Showing %s.", err,
                     cached ? "the copy from last time" : "links.txt from the SD card");
        }
        free(txt);
    }
    Link *links = NULL;
    int n = links_load(listPath, &links);
    if (n <= 0) {
        free(links);
        rebuild_items();
        build_view();
        return n;
    }
    entries = calloc((size_t)n, sizeof(Entry));
    if (!entries) {
        free(links);
        rebuild_items();
        return 0;
    }
    for (int i = 0; i < n; i++) {
        entries[i].link = links[i];
        url_hash(links[i].url, entries[i].hash);
    }
    free(links);
    entryCount = n;
    load_installed();
    scan_tmp_dir();
    double lastDraw = now_s();
    for (int i = 0; i < n; i++) {
        refresh_status(&entries[i]);
        prepare_cover(&entries[i]);
        if (now_s() - lastDraw > 0.1) {
            show_loading(i, n);
            lastDraw = now_s();
        }
    }
    gTmpSetValid = false; /* later refreshes (after downloads) check the SD card properly */
    load_lists();
    rebuild_items();
    build_view();
    gWinCenter = -100000;
    return n;
}

/* ------------------------------------------------------------------ screens */

/* First letter of a title for L/R jumping ('#' for digits and symbols). */
static char title_letter(int pos)
{
    int i = gView[pos];
    const char *t = items && items[i].title ? items[i].title : entries[i].link.name;
    for (; *t; t++) {
        unsigned char c = (unsigned char)*t;
        if (c >= 'a' && c <= 'z') return (char)(c - 32);
        if (c >= 'A' && c <= 'Z') return (char)c;
        if (c >= '0' && c <= '9') return '#';
    }
    return '#';
}

/* Next (dir > 0) or previous letter group: works on lists sorted by title, like the scraper writes. */
static int letter_jump(int sel, int dir)
{
    if (gViewCount == 0) return 0;
    char cur = title_letter(sel);
    int i = sel;
    if (dir > 0) {
        while (i < gViewCount - 1 && title_letter(i) == cur) i++;
        return i;
    }
    while (i > 0 && title_letter(i) == cur) i--; /* end of the previous group */
    char prev = title_letter(i);
    while (i > 0 && title_letter(i - 1) == prev) i--; /* its first entry */
    return i;
}

static void draw_list_scene(void)
{
    ui_begin_frame(now_s());
    ui_header(gSdText, gMusicState);
    if (entryCount > 0 && gQuery[0]) ui_search_bar(gQuery, gViewCount, entryCount);
    if (entryCount > 0 && gViewCount == 0 && gQuery[0]) {
        char body[300];
        snprintf(body, sizeof(body), "Nothing in your list matches \"%s\". Press - to search again, or B to show everything.",
                 gQuery);
        ui_empty("No matches", body, NULL);
        UiHint hints[3] = {{"-", "Search"}, {"B", "Clear"}, {"+", "Menu"}};
        ui_footer(hints, 3, NULL);
    } else if (entryCount > 0 && gViewCount > 0) {
        ui_list(view_items(), gViewCount, gSel);
        int ce = cur_entry();
        UiHint hints[5] = {{"A", "Details"},
                           {"Y", ce >= 0 && queue_pos(ce) ? "Unqueue" : "Queue"},
                           {"X", ce >= 0 && entries[ce].fav ? "Unfavorite" : "Favorite"},
                           {"-", "Search"},
                           {"+", "Menu"}};
        char right[64];
        if (gShow == SHOW_QUEUED) snprintf(right, sizeof(right), "L/R: move in queue    %d / %d", gSel + 1, gViewCount);
        else if (gQueueCount) snprintf(right, sizeof(right), "Queue: %d    %d / %d", gQueueCount, gSel + 1, gViewCount);
        else snprintf(right, sizeof(right), "%d / %d", gSel + 1, gViewCount);
        ui_footer(hints, 5, right);
    } else {
        bool filtered = gShow != SHOW_ALL && entryCount > 0;
        if (filtered)
            ui_empty("Nothing to show", "No games match the Show filter. Press + to change it.", NULL);
        else
            ui_empty("No links yet",
                     "Put the SD card in your PC and add one direct download link per line to links.txt, then "
                     "choose Reload in the + menu.",
                     "sd:/wiiu/apps/wiidrop/links.txt");
        UiHint hints[1] = {{"+", "Menu"}};
        ui_footer(hints, 1, NULL);
    }
}

/* Modal message over the list. Returns when A (or a tap) is pressed, or the app is closing. */
static void message_box(UiMsgKind kind, const char *title, const char *body)
{
    if (gBatch) { /* running the queue: remember the error, don't stop for it */
        if (kind == UI_MSG_ERROR) snprintf(gBatchErr, sizeof(gBatchErr), "%s: %s", title, body ? body : "");
        return;
    }
    while (!gQuit) {
        pump();
        cover_window_update();
        Input in = read_input();
        if ((in.pressed & VPAD_BUTTON_A) || in.tap) return;
        if (!gVisible) continue;
        draw_list_scene();
        ui_message(kind, title, body, NULL);
        ui_end_frame();
    }
}

static char gBatchNote[160];

static void show_progress(const UiProgress *p)
{
    if (!gVisible) return;
    UiProgress q = *p;
    if (gBatch && gBatchNote[0]) q.note = gBatchNote;
    ui_begin_frame(now_s());
    ui_header(gSdText, gMusicState);
    ui_progress(&q);
    ui_end_frame();
}

/* HOME can't be allowed to suspend us mid-write (the GPU memory would go away under the
   progress screen), so it's switched off while a transfer runs. */
/* The progress screen is drawn from inside the download loop, so every redraw pauses the network.
   During transfers: redraw 10x a second and don't wait for the screen refresh (vsync) to present. */
#define PROGRESS_INTERVAL (1.0 / 10) /* the UI shares the CPU with the transfer: fewer, lighter frames */

static void transfer_begin(void)
{
    ui_set_low_power(true);
    SDL_RenderSetVSync(gRenderer, 0);
    OSEnableHomeButtonMenu(FALSE);
    covers_pause(1);
    IM_SetRuntimeParameter(IM_PARAMETER_APD_ENABLED, 0); /* no auto power-down mid-download */
}

static void transfer_end(void)
{
    ui_set_low_power(false);
    SDL_RenderSetVSync(gRenderer, 1);
    OSEnableHomeButtonMenu(TRUE);
    covers_pause(0);
}

/* ------------------------------------------------------------------ download */

static const char *kind_error(DiscKind k)
{
    if (k == DISC_WII_ISO) return "This Wii disc image couldn't be converted to WBFS.";
    return "This link isn't a Wii .wbfs / .iso / .rvz, a GameCube .iso / .rvz, or a .7z / .zip. "
           "If it's a share link (Drive, OneDrive, Mega...) it probably returns a web page. "
           "Use a direct download link.";
}

static void conv_state_path(const char *hash, char *out, size_t n)
{
    snprintf(out, n, "%s/%s/convert.state", gTmpDir, hash);
}

/* Looks at the first bytes of a stream before anything is written, so we never write gigabytes of
   an HTML page, and so we know whether to store it as-is or convert it (Wii ISO -> WBFS). */
typedef struct {
    uint8_t hdr[DISC_HEADER_NEED];
    size_t len;
    bool done;
    bool allow7z;
    bool is7z;
    bool allowZip;
    bool isZip;
    bool allowRvz;
    bool isRvz;
    bool isWiiIso;
} HeaderCheck;

/* Returns how many bytes of data it consumed into the header buffer, or -1 if the content is wrong. */
static long header_feed(HeaderCheck *h, const void *data, size_t len, char *err, size_t errlen)
{
    if (h->done) return 0;
    size_t take = DISC_HEADER_NEED - h->len;
    if (take > len) take = len;
    memcpy(h->hdr + h->len, data, take);
    h->len += take;
    if (h->len < DISC_HEADER_NEED) return (long)take;

    h->done = true;
    if (h->allow7z && sz_is_7z(h->hdr, h->len)) {
        h->is7z = true;
        return (long)take;
    }
    if (h->allowZip && zs_is_zip(h->hdr, h->len)) {
        h->isZip = true;
        return (long)take;
    }
    if (h->allowRvz && rvz_is_rvz(h->hdr, h->len)) {
        h->isRvz = true;
        return (long)take;
    }
    DiscInfo info;
    DiscKind k = disc_identify(h->hdr, h->len, &info);
    if (k == DISC_WII_ISO) h->isWiiIso = true;
    if (k == DISC_WII_WBFS || k == DISC_GC_ISO || k == DISC_WII_ISO) return (long)take;
    snprintf(err, errlen, "%s", kind_error(k));
    return -1;
}

typedef enum { MODE_RAW, MODE_ARCHIVE, MODE_CONVERT, MODE_RVZ } Mode;

static void set_stages(UiProgress *p, Mode mode, int stage)
{
    p->stages[0] = mode == MODE_CONVERT ? "Download + convert" : "Download";
    if (mode == MODE_ARCHIVE || mode == MODE_RVZ) {
        p->stages[1] = mode == MODE_RVZ ? "Rebuild" : "Extract";
        p->stages[2] = "Install";
        p->stageCount = 3;
    } else {
        p->stages[1] = "Install";
        p->stages[2] = NULL;
        p->stageCount = 2;
    }
    p->stage = stage;
}

/* ---- game writer: header check, then WBFS conversion (Wii ISO) or a straight copy, into the
        extract folder. Used behind the zip reader. ---- */

#define RAW_SAVE_EVERY (16ull * 1024 * 1024)

typedef struct {
    SplitWriter out;
    WbfsConv conv;
    HeaderCheck hc;
    bool convert, opened;
    char err[256];
    void (*onSave)(void *ud, uint64_t pos); /* output up to pos is safely on disk */
    void *ud;
    uint64_t lastRawSave;
    bool rvz;     /* the file inside is an .rvz: kept as-is in the staging folder, rebuilt after the download */
    char hash[9];
} GameWriter;

static void gw_init(GameWriter *g, const char *hash, void (*onSave)(void *, uint64_t), void *ud)
{
    memset(g, 0, sizeof(*g));
    snprintf(g->hash, sizeof(g->hash), "%s", hash);
    g->hc.allowRvz = true;
    extract_writer(hash, &g->out);
    char statePath[400];
    conv_state_path(hash, statePath, sizeof(statePath));
    wc_init(&g->conv, &g->out, statePath);
    g->conv.onSave = onSave;
    g->conv.onSaveUd = ud;
    g->onSave = onSave;
    g->ud = ud;
}

static int gw_out(GameWriter *g, const void *data, size_t len)
{
    if (len == 0) return 0;
    if (g->convert) {
        if (wc_write(&g->conv, data, len) == 0) return 0;
        snprintf(g->err, sizeof(g->err), "%s", g->conv.err);
        return -1;
    }
    if (sw_write(&g->out, data, len) != 0) {
        snprintf(g->err, sizeof(g->err), "Writing to the SD card failed (full or removed?)");
        return -1;
    }
    if (g->onSave && g->out.pos - g->lastRawSave >= RAW_SAVE_EVERY) {
        if (g->out.fp) fflush(g->out.fp);
        g->lastRawSave = g->out.pos;
        g->onSave(g->ud, g->out.pos);
    }
    return 0;
}

static int gw_write(GameWriter *g, const void *data, size_t len)
{
    const uint8_t *p = data;
    if (!g->hc.done) {
        long used = header_feed(&g->hc, p, len, g->err, sizeof(g->err));
        if (used < 0) return -1;
        if (!g->hc.done) return 0;
        g->convert = g->hc.isWiiIso;
        if (g->hc.isRvz) { /* store the .rvz raw; it needs random access to rebuild */
            g->rvz = true;
            staging_writer(g->hash, &g->out);
        }
        if ((g->convert ? wc_open(&g->conv, 0) : sw_open(&g->out, 0)) != 0) {
            snprintf(g->err, sizeof(g->err), "Could not create files on the SD card.");
            return -1;
        }
        g->opened = true;
        if (gw_out(g, g->hc.hdr, DISC_HEADER_NEED) != 0) return -1;
        p += used;
        len -= (size_t)used;
    }
    return gw_out(g, p, len);
}

/* Continue writing at pos (the ISO offset wc_resume_point() gave, or the raw file's size). */
static int gw_resume(GameWriter *g, bool convert, bool rvz, uint64_t pos)
{
    if (rvz) {
        g->rvz = true;
        g->hc.isRvz = true;
        staging_writer(g->hash, &g->out);
    }
    g->hc.done = true;
    g->hc.isWiiIso = convert;
    g->convert = convert;
    g->lastRawSave = pos;
    if ((convert ? wc_open(&g->conv, pos) : sw_open(&g->out, pos)) != 0) return -1;
    g->opened = true;
    return 0;
}

/* Save progress right now (before pausing). */
static void gw_checkpoint(GameWriter *g)
{
    if (!g->opened) return;
    if (g->convert) {
        wc_save(&g->conv);
    } else if (g->onSave) {
        if (g->out.fp) fflush(g->out.fp);
        g->onSave(g->ud, g->out.pos);
    }
}

static int gw_finish(GameWriter *g)
{
    if (!g->opened) {
        snprintf(g->err, sizeof(g->err), "The game file in the archive is empty.");
        return -1;
    }
    if (g->convert ? wc_finish(&g->conv) : sw_close(&g->out)) {
        snprintf(g->err, sizeof(g->err), "%s", g->convert ? g->conv.err : "Could not finish writing to the SD card.");
        return -1;
    }
    return 0;
}

static void gw_close(GameWriter *g)
{
    if (!g->opened) return;
    if (g->convert) wc_close(&g->conv);
    else sw_close(&g->out);
}

static void gw_discard(GameWriter *g)
{
    wc_discard(&g->conv);
    sw_close(&g->out);
    sw_remove_all(&g->out);
}

static void zip_ckpt_path(const char *hash, char *out, size_t n)
{
    snprintf(out, n, "%s/%s/zip.ckpt", gTmpDir, hash);
}

/* ---- checksum of the downloaded file (optional, from links.txt: "Name | URL | md5=...") ---- */

enum { SUM_NONE, SUM_MD5, SUM_SHA1, SUM_CRC32 };

typedef struct {
    int kind;
    bool valid;   /* every byte from 0 to pos went through the hash */
    uint64_t pos;
    mbedtls_md5_context md5;
    mbedtls_sha1_context sha1;
    uLong crc;
} Sum;

static Sum gSum;

static int sum_kind(const Link *l)
{
    return !strcmp(l->sumType, "md5") ? SUM_MD5 : !strcmp(l->sumType, "sha1") ? SUM_SHA1
         : !strcmp(l->sumType, "crc32") ? SUM_CRC32 : SUM_NONE;
}

static void sum_reset(Sum *h, int kind)
{
    h->kind = kind;
    h->valid = kind != SUM_NONE;
    h->pos = 0;
    mbedtls_md5_init(&h->md5);
    mbedtls_md5_starts_ret(&h->md5);
    mbedtls_sha1_init(&h->sha1);
    mbedtls_sha1_starts_ret(&h->sha1);
    h->crc = crc32(0L, Z_NULL, 0);
}

static void sum_update(Sum *h, const void *data, size_t len)
{
    if (!h->valid || !len) return;
    if (h->kind == SUM_MD5) mbedtls_md5_update_ret(&h->md5, data, len);
    else if (h->kind == SUM_SHA1) mbedtls_sha1_update_ret(&h->sha1, data, len);
    else if (h->kind == SUM_CRC32) h->crc = crc32(h->crc, data, (uInt)len);
    h->pos += len;
}

static void sum_hex(Sum *h, char out[41])
{
    uint8_t d[20];
    int n = 0;
    if (h->kind == SUM_MD5) {
        mbedtls_md5_finish_ret(&h->md5, d);
        n = 16;
    } else if (h->kind == SUM_SHA1) {
        mbedtls_sha1_finish_ret(&h->sha1, d);
        n = 20;
    } else {
        d[0] = (uint8_t)(h->crc >> 24);
        d[1] = (uint8_t)(h->crc >> 16);
        d[2] = (uint8_t)(h->crc >> 8);
        d[3] = (uint8_t)h->crc;
        n = 4;
    }
    for (int i = 0; i < n; i++) snprintf(out + i * 2, 3, "%02x", d[i]);
    out[n * 2] = '\0';
}

/* ---- step 1: download into staging (converting Wii ISOs on the fly) ---- */

typedef struct {
    Entry *e;
    SplitWriter sw;      /* raw staging: .wbfs / GameCube .iso / .7z */
    SplitWriter convOut; /* converted WBFS (Wii ISO links) */
    WbfsConv conv;
    bool convert;
    bool opened;
    bool guess7z;
    bool guessRvz;
    /* .zip links: unzipped (and converted) while downloading, straight into the extract folder */
    bool zipMode;
    ZipStream *zip;
    GameWriter gw;
    char zipCkpt[400];
    char zipEntry[256];
    uint64_t start;
    uint64_t expected; /* full file size if the server told us, else 0 */
    HeaderCheck hc;
    bool badContent;
    char err[256];
    char detail[400];
    double lastDraw;
} DlState;

static int dl_open_sink(DlState *s)
{
    if (s->convert) {
        if (wc_open(&s->conv, s->start) != 0) {
            snprintf(s->err, sizeof(s->err), "%s", s->conv.err);
            return -1;
        }
    } else if (sw_open(&s->sw, s->start) != 0) {
        snprintf(s->err, sizeof(s->err), "Could not create files on the SD card.");
        return -1;
    }
    s->opened = true;
    return 0;
}

static int dl_sink_write(DlState *s, const void *data, size_t len)
{
    if (len == 0) return 0;
    if (s->convert) {
        if (wc_write(&s->conv, data, len) == 0) return 0;
        snprintf(s->err, sizeof(s->err), "%s", s->conv.err);
        return -1;
    }
    if (sw_write(&s->sw, data, len) == 0) return 0;
    snprintf(s->err, sizeof(s->err), "Writing to the SD card failed (full or removed?)");
    return -1;
}

static void dl_zip_saved(void *ud, uint64_t pos)
{
    DlState *s = ud;
    if (!s->zip) return;
    ZipCkpt *ck = malloc(sizeof(ZipCkpt));
    if (!ck) return;
    if (zs_checkpoint(s->zip, pos, ck) == 0) {
        ck->userFlag = (s->gw.convert ? 1 : 0) | (s->gw.rvz ? 2 : 0);
        zs_save_ckpt(s->zipCkpt, ck);
    }
    free(ck);
}

static int zsink_begin(void *ud, const char *name, uint64_t size)
{
    DlState *s = ud;
    snprintf(s->zipEntry, sizeof(s->zipEntry), "%s", name);
    uint64_t freeBytes = sd_free_bytes();
    if (freeBytes && size > freeBytes) {
        char need[32], have[32];
        fmt_size(need, sizeof(need), size);
        fmt_size(have, sizeof(have), freeBytes);
        snprintf(s->err, sizeof(s->err), "Not enough space on the SD card: need %s, only %s free.", need, have);
        return -1;
    }
    return 0;
}

static int zsink_write(void *ud, const void *data, size_t len)
{
    DlState *s = ud;
    if (gw_write(&s->gw, data, len) == 0) return 0;
    snprintf(s->err, sizeof(s->err), "%s", s->gw.err);
    return -1;
}

static int dl_start_zip(DlState *s)
{
    ZipSink zsk = {zsink_begin, zsink_write, s};
    s->zip = zs_create(&zsk);
    if (!s->zip) {
        snprintf(s->err, sizeof(s->err), "Out of memory");
        return -1;
    }
    s->zipMode = true;
    return 0;
}

static int dl_zip_feed(DlState *s, const void *data, size_t len)
{
    if (len == 0 || zs_feed(s->zip, data, len) == 0) return 0;
    if (!s->err[0]) snprintf(s->err, sizeof(s->err), "%s", zs_error(s->zip));
    return -1;
}

static int dl_begin(void *ud, int resumed, uint64_t length)
{
    DlState *s = ud;
    if (!resumed && s->start > 0) {
        /* the server ignored our Range request: start over from byte 0 */
        if (s->convert) wc_discard(&s->conv);
        sw_remove_all(&s->sw);
        if (s->zipMode) {
            gw_discard(&s->gw);
            zs_destroy(s->zip);
            s->zip = NULL;
            s->zipMode = false;
            remove(s->zipCkpt);
            gw_init(&s->gw, s->e->hash, dl_zip_saved, s);
        }
        s->start = 0;
        s->convert = false;
        if (gSum.kind) sum_reset(&gSum, gSum.kind); /* starting over: so does the checksum */
        memset(&s->hc, 0, sizeof(s->hc));
        s->hc.allow7z = true;
        s->hc.allowZip = true;
        s->hc.allowRvz = true;
    }
    s->expected = length ? s->start + length : 0;

    uint64_t freeBytes = sd_free_bytes();
    if (freeBytes && length > freeBytes) {
        char need[32], have[32];
        fmt_size(need, sizeof(need), length);
        fmt_size(have, sizeof(have), freeBytes);
        snprintf(s->err, sizeof(s->err), "Not enough space on the SD card: need %s, only %s free.", need, have);
        return -1;
    }
    /* resuming: we already know what this is, so open the right writer now */
    if (s->hc.done && !s->zipMode) return dl_open_sink(s);
    return 0;
}

static int dl_write(void *ud, const void *data, size_t len)
{
    DlState *s = ud;
    const uint8_t *p = data;
    sum_update(&gSum, data, len);
    if (s->zipMode) return dl_zip_feed(s, p, len);
    if (!s->hc.done) {
        long used = header_feed(&s->hc, p, len, s->err, sizeof(s->err));
        if (used < 0) {
            s->badContent = true;
            return -1;
        }
        if (!s->hc.done) return 0; /* still collecting the header */
        if (s->hc.isZip) {
            sw_remove_all(&s->sw);
            if (dl_start_zip(s) != 0 || dl_zip_feed(s, s->hc.hdr, DISC_HEADER_NEED) != 0) return -1;
            return dl_zip_feed(s, p + used, len - (size_t)used);
        }
        s->convert = s->hc.isWiiIso;
        if (s->convert) sw_remove_all(&s->sw); /* no raw copy needed */
        if (dl_open_sink(s) != 0 || dl_sink_write(s, s->hc.hdr, DISC_HEADER_NEED) != 0) return -1;
        p += used;
        len -= (size_t)used;
    }
    return dl_sink_write(s, p, len);
}

static int dl_progress(void *ud, uint64_t done, uint64_t total, uint64_t bps)
{
    DlState *s = ud;
    double t = now_s();
    if (t - s->lastDraw < PROGRESS_INTERVAL) return 0;
    s->lastDraw = t;

    pump();
    if (gQuit) return 1;
    if (read_input().pressed & VPAD_BUTTON_B) return 1;

    bool converting = s->convert || (s->zipMode && s->gw.convert);
    Mode mode = (s->zipMode && s->gw.rvz) ? MODE_RVZ
                : converting ? MODE_CONVERT
                : (s->hc.done ? s->hc.isRvz : s->guessRvz) ? MODE_RVZ
                : (s->hc.done ? s->hc.is7z : s->guess7z)   ? MODE_ARCHIVE
                                                           : MODE_RAW;
    UiProgress p = {0};
    set_stages(&p, mode, 0);
    p.title = s->e->display[0] ? s->e->display : s->e->link.name;
    p.detail = s->detail;
    p.done = done;
    p.total = total;
    p.bytesPerSec = bps;
    p.hint = "Pause  (progress is kept)";
    const char *what = (s->zipMode && s->gw.rvz) ? "Unzipping the .rvz as it downloads"
                       : s->zipMode ? (converting ? "Unzipping + converting to WBFS as it downloads" : "Unzipping as it downloads")
                       : s->convert ? "Converting to WBFS as it downloads"
                                    : "HOME is disabled until this finishes";
    if (gBatch) batch_note(done, total, bps);
    static char note[200];
    snprintf(note, sizeof(note), "%s  -  %s", net_status()[0] ? net_status() : "Connecting", what);
    p.note = note;
    show_progress(&p);
    return 0;
}

/* ---- step 2 (archives only): stream the game out of the .7z (converting a Wii ISO inside) ---- */

typedef struct {
    Entry *e;
    SplitWriter sw;
    WbfsConv conv;
    bool convert;
    bool opened;
    HeaderCheck hc;
    char name[256];
    char detail[300];
    char err[256];
    double startTime, lastDraw;
} ExState;

static int ex_begin(void *ud, const char *name, uint64_t size)
{
    ExState *x = ud;
    snprintf(x->name, sizeof(x->name), "%s", name);
    uint64_t freeBytes = sd_free_bytes();
    if (freeBytes && size > freeBytes) {
        char need[32], have[32];
        fmt_size(need, sizeof(need), size);
        fmt_size(have, sizeof(have), freeBytes);
        snprintf(x->err, sizeof(x->err),
                 "Not enough space to extract: need %s, only %s free. The .7z is kept until the game is out.", need,
                 have);
        return -1;
    }
    x->startTime = now_s();
    return 0;
}

static int ex_sink_write(ExState *x, const void *data, size_t len)
{
    if (len == 0) return 0;
    if (x->convert) {
        if (wc_write(&x->conv, data, len) == 0) return 0;
        snprintf(x->err, sizeof(x->err), "%s", x->conv.err);
        return -1;
    }
    if (sw_write(&x->sw, data, len) == 0) return 0;
    snprintf(x->err, sizeof(x->err), "Writing to the SD card failed (full or removed?)");
    return -1;
}

static int ex_write(void *ud, const void *data, size_t len)
{
    ExState *x = ud;
    const uint8_t *p = data;
    if (!x->hc.done) {
        long used = header_feed(&x->hc, p, len, x->err, sizeof(x->err));
        if (used < 0) return -1;
        if (!x->hc.done) return 0;
        x->convert = x->hc.isWiiIso;
        int rc = x->convert ? wc_open(&x->conv, 0) : sw_open(&x->sw, 0);
        if (rc != 0) {
            snprintf(x->err, sizeof(x->err), "Could not create files on the SD card.");
            return -1;
        }
        x->opened = true;
        if (ex_sink_write(x, x->hc.hdr, DISC_HEADER_NEED) != 0) return -1;
        p += used;
        len -= (size_t)used;
    }
    return ex_sink_write(x, p, len);
}

static int ex_progress(void *ud, uint64_t done, uint64_t total)
{
    ExState *x = ud;
    double t = now_s();
    if (t - x->lastDraw < PROGRESS_INTERVAL) return 0;
    x->lastDraw = t;

    pump();
    if (gQuit) return 1;
    if (read_input().pressed & VPAD_BUTTON_B) return 1;

    double secs = t - x->startTime;
    UiProgress p = {0};
    set_stages(&p, MODE_ARCHIVE, 1);
    p.title = x->name;
    snprintf(x->detail, sizeof(x->detail), "from %s", x->e->link.name);
    p.detail = x->detail;
    p.done = done;
    p.total = total;
    p.bytesPerSec = secs > 0.5 ? (uint64_t)(done / secs) : 0;
    p.hint = "Cancel  (the .7z is kept)";
    p.note = x->convert ? "Converting the Wii ISO to WBFS as it's extracted  -  HOME is disabled"
                        : "HOME is disabled until this finishes";
    show_progress(&p);
    return 0;
}

/* Returns 0 on success (game is in the extract folder), 1 cancelled, -1 error (message shown). */
static int extract_archive(Entry *e)
{
    static ExState x; /* off the stack */
    memset(&x, 0, sizeof(x));
    x.e = e;
    extract_writer(e->hash, &x.sw);
    char statePath[400];
    snprintf(statePath, sizeof(statePath), "%s/%s/extract.state", gTmpDir, e->hash);
    wc_init(&x.conv, &x.sw, statePath);

    SplitWriter archive;
    staging_writer(e->hash, &archive);

    UiProgress p = {0};
    set_stages(&p, MODE_ARCHIVE, 1);
    p.title = "Opening the .7z...";
    p.detail = e->link.name;
    show_progress(&p);

    char err[256] = {0};
    SzSink sink = {ex_begin, ex_write, ex_progress, &x};
    int rc = sz_extract_game(&archive, &sink, err, sizeof(err));
    if (x.opened) {
        if (rc == 0) {
            int frc = x.convert ? wc_finish(&x.conv) : sw_close(&x.sw);
            if (frc != 0) {
                rc = -1;
                snprintf(x.err, sizeof(x.err), "%s", x.convert ? x.conv.err : "Could not finish writing to the SD card.");
            }
        }
        if (rc != 0) { /* restart extraction from scratch next time */
            if (x.convert) wc_discard(&x.conv);
            else {
                sw_close(&x.sw);
                sw_remove_all(&x.sw);
            }
        }
    }
    remove(statePath);

    if (gQuit) return 1;
    if (rc == 1) {
        message_box(UI_MSG_INFO, "Extraction cancelled",
                    "The .7z is still on the SD card. Pick the game again to retry.");
    } else if (rc != 0) {
        message_box(UI_MSG_ERROR, "Extraction failed", x.err[0] ? x.err : err[0] ? err : "Unknown error.");
    }
    return rc;
}

/* ---- step 2 (RVZ only): rebuild the disc from the downloaded .rvz ---- */

typedef struct {
    SplitWriter sw;
    FILE *fp;
    int part;
} SplitRead;

static int split_read_at(void *ud, uint64_t off, void *buf, size_t len)
{
    SplitRead *r = ud;
    uint8_t *p = buf;
    while (len > 0) {
        int part = (int)(off / r->sw.part_size);
        uint64_t in = off % r->sw.part_size;
        if (part != r->part || !r->fp) {
            if (r->fp) fclose(r->fp);
            char path[600];
            sw_part_path(&r->sw, part, path, sizeof(path));
            r->fp = fopen(path, "rb");
            r->part = part;
            if (!r->fp) return -1;
        }
        size_t n = (uint64_t)len < r->sw.part_size - in ? len : (size_t)(r->sw.part_size - in);
        if (fseeko(r->fp, (off_t)in, SEEK_SET) != 0 || fread(p, 1, n, r->fp) != n) return -1;
        p += n;
        off += n;
        len -= n;
    }
    return 0;
}

typedef struct {
    Entry *e;
    GameWriter gw;
    double start, lastDraw;
    char detail[300];
} RvzState;

static int rvz_out(void *ud, const void *data, size_t len)
{
    RvzState *x = ud;
    return gw_write(&x->gw, data, len);
}

static int rvz_progress(void *ud, uint64_t done, uint64_t total)
{
    RvzState *x = ud;
    double t = now_s();
    if (t - x->lastDraw < PROGRESS_INTERVAL) return 0;
    x->lastDraw = t;
    pump();
    if (gQuit) return 1;
    if (read_input().pressed & VPAD_BUTTON_B) return 1;
    double secs = t - x->start;
    UiProgress p = {0};
    set_stages(&p, MODE_RVZ, 1);
    p.title = x->e->display[0] ? x->e->display : x->e->link.name;
    p.detail = x->detail;
    p.done = done;
    p.total = total;
    p.bytesPerSec = secs > 0.5 ? (uint64_t)(done / secs) : 0;
    p.hint = "Cancel  (the .rvz is kept)";
    p.note = x->gw.convert ? "Rebuilding the disc and converting it to WBFS  -  HOME is disabled"
                           : "Rebuilding the disc  -  HOME is disabled until this finishes";
    show_progress(&p);
    return 0;
}

/* Returns 0 on success (game is in the extract folder), 1 cancelled, -1 error (message shown). */
static int convert_rvz(Entry *e)
{
    static RvzState x; /* off the stack: holds a WBFS converter */
    memset(&x, 0, sizeof(x));
    x.e = e;
    gw_init(&x.gw, e->hash, NULL, NULL);
    x.gw.hc.allowRvz = false;
    /* Own progress file: convert.state means "an interrupted direct .iso download" to download_entry(),
       which would otherwise mistake a crashed RVZ rebuild for it (and resume the wrong thing). */
    snprintf(x.gw.conv.statePath, sizeof(x.gw.conv.statePath), "%s/%s/rvz.state", gTmpDir, e->hash);
    SplitRead rd;
    memset(&rd, 0, sizeof(rd));
    staging_writer(e->hash, &rd.sw);
    rd.part = -1;

    UiProgress p = {0};
    set_stages(&p, MODE_RVZ, 1);
    p.title = e->display[0] ? e->display : e->link.name;
    p.detail = "Opening the .rvz...";
    show_progress(&p);

    char err[256] = {0};
    int rc = -1;
    Rvz *r = rvz_open(split_read_at, &rd, err, sizeof(err));
    if (r) {
        uint64_t need = rvz_disc_size(r), freeBytes = sd_free_bytes();
        if (freeBytes && need > freeBytes) {
            char a[32], b[32];
            fmt_size(a, sizeof(a), need);
            fmt_size(b, sizeof(b), freeBytes);
            snprintf(err, sizeof(err), "Not enough space to rebuild the disc: need up to %s, only %s free. "
                                       "The .rvz is kept.", a, b);
        } else {
            snprintf(x.detail, sizeof(x.detail), "%s disc from %s", rvz_is_wii(r) ? "Wii" : "GameCube",
                     e->link.name);
            x.start = now_s();
            rc = rvz_extract(r, rvz_out, rvz_progress, &x, err, sizeof(err));
            if (rc == 0 && gw_finish(&x.gw) != 0) {
                rc = -1;
                snprintf(err, sizeof(err), "%s", x.gw.err);
            }
            if (rc == -1 && !err[0]) snprintf(err, sizeof(err), "%s", x.gw.err);
        }
        rvz_close(r);
    }
    if (rd.fp) fclose(rd.fp);
    if (rc != 0) gw_discard(&x.gw); /* the .rvz stays; rebuilding restarts next time */

    if (gQuit) return 1;
    if (rc == 1) {
        message_box(UI_MSG_INFO, "Cancelled", "The .rvz is still on the SD card. Pick the game again to retry.");
    } else if (rc != 0) {
        message_box(UI_MSG_ERROR, "Couldn't rebuild the disc", err[0] ? err : "Unknown error.");
    }
    return rc;
}

/* ---- step 3: move the finished game into the loader folder ---- */

static int finalize(Entry *e, const SplitWriter *from, char *err, size_t errlen)
{
    SplitWriter to;
    char a[600], b[600];
    sw_part_path(from, 0, a, sizeof(a));
    DiscInfo info;
    DiscKind k = disc_identify_file(a, &info);
    if (k != DISC_WII_WBFS && k != DISC_GC_ISO) {
        snprintf(err, errlen, "%s", kind_error(k));
        return -1;
    }
    bool gc = (k == DISC_GC_ISO);
    int disc = gc ? info.disc : 0;
    char folder[96];
    make_folder_name(info.title[0] ? info.title : e->link.name, info.id, folder, sizeof(folder));
    if (gc) { /* the other disc of this game is already installed: use its folder, whatever it's called */
        for (int i = 0; i < entryCount; i++) {
            Entry *o = &entries[i];
            if (o != e && o->gc && o->folder[0] && o->usb == gUseUsb && !strcmp(o->id, info.id) && o->disc != disc) {
                snprintf(folder, sizeof(folder), "%s", o->folder);
                break;
            }
        }
    }
    final_writer(gUseUsb, gc, disc, folder, info.id, &to);

    char dir[600];
    snprintf(dir, sizeof(dir), "%s", to.base);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = '\0';
    mkdir_p(dir);
    sw_remove_all(&to); /* replace an older copy, keep any other files (covers etc.) */

    for (int part = 0; part < 64; part++) {
        sw_part_path(from, part, a, sizeof(a));
        if (!file_exists(a)) break;
        if (gc && part > 0) {
            snprintf(err, errlen, "GameCube image is bigger than 4 GB?");
            return -1;
        }
        sw_part_path(&to, part, b, sizeof(b)); /* -> <ID>.wbfs / <ID>.wbf1 or game.iso */
        if (rename(a, b) != 0) {
            snprintf(err, errlen, "Could not move files out of the tmp folder.");
            return -1;
        }
    }

    e->gc = gc;
    e->disc = disc;
    e->usb = gUseUsb;
    snprintf(e->id, sizeof(e->id), "%s", info.id);
    snprintf(e->folder, sizeof(e->folder), "%s", folder);
    save_installed();
    return 0;
}

#define MAX_RETRIES 20

/* Network failures are worth retrying; a server saying "no" (404, 403, 401...) isn't. */
static bool retryable(const char *err)
{
    if (strstr(err, "certificate")) return false; /* won't fix itself: needs a config change */
    const char *http = strstr(err, "returned error: ");
    if (http) {
        int code = atoi(http + 16);
        return code >= 500 || code == 408 || code == 429;
    }
    return true;
}

/* "Connection lost, retrying in N s". Returns true to retry, false if the user pressed B. */
static bool retry_wait(Entry *e, int attempt, const char *why, uint64_t done, uint64_t total)
{
    static const int delays[] = {5, 10, 20, 30};
    int secs = attempt <= 4 ? delays[attempt - 1] : 60;
    double until = now_s() + secs;
    char detail[300], note[200];
    snprintf(detail, sizeof(detail), "Connection lost: %s", why && why[0] ? why : "network error");
    while (!gQuit) {
        pump();
        if (read_input().pressed & VPAD_BUTTON_B) return false;
        double left = until - now_s();
        if (left <= 0) return true;
        UiProgress p = {0};
        set_stages(&p, MODE_RAW, 0);
        p.title = e->display[0] ? e->display : e->link.name;
        p.detail = detail;
        p.done = done;
        p.total = total > done ? total : 0;
        snprintf(note, sizeof(note), "Retrying in %d s (attempt %d of %d)  -  your progress is kept", (int)left + 1,
                 attempt + 1, MAX_RETRIES + 1);
        p.note = note;
        p.hint = "Stop retrying  (progress is kept)";
        show_progress(&p);
        SDL_Delay(30);
    }
    return false;
}

/* Hash what an interrupted raw download already saved, so the checksum covers the whole file. */
static bool sum_prehash(Entry *e, uint64_t upTo)
{
    SplitRead rd;
    memset(&rd, 0, sizeof(rd));
    staging_writer(e->hash, &rd.sw);
    rd.part = -1;
    uint8_t *buf = malloc(1024 * 1024);
    bool ok = buf != NULL;
    double last = 0;
    for (uint64_t at = 0; ok && at < upTo && !gQuit;) {
        size_t n = upTo - at < 1024 * 1024 ? (size_t)(upTo - at) : 1024 * 1024;
        if (split_read_at(&rd, at, buf, n) != 0) {
            ok = false;
            break;
        }
        sum_update(&gSum, buf, n);
        at += n;
        if (now_s() - last > PROGRESS_INTERVAL) {
            last = now_s();
            pump();
            UiProgress p = {0};
            set_stages(&p, MODE_RAW, 0);
            p.title = e->display[0] ? e->display : e->link.name;
            p.detail = "Checking the part that's already downloaded (for the checksum)...";
            p.done = at;
            p.total = upTo;
            show_progress(&p);
        }
    }
    if (rd.fp) fclose(rd.fp);
    free(buf);
    return ok && !gQuit;
}

/* 0 installed, 1 paused / cancelled, -1 failed (message already shown, or kept in gBatchErr). */
static int download_entry(const Config *cfg, Entry *e)
{
    static DlState s; /* ~20 KB: keep it off the stack */
    int result = -1;
    char sumNote[120] = "";
    char statePath[400];
    conv_state_path(e->hash, statePath, sizeof(statePath));
    char host[128], file[200];
    url_parts(e->link.url, host, sizeof(host), file, sizeof(file));
    char stage[400];
    snprintf(stage, sizeof(stage), "%s/%s", gTmpDir, e->hash);
    mkdir_p(stage);
    char readyPath[420];
    snprintf(readyPath, sizeof(readyPath), "%s/rvz.ready", stage);

    /* An .rvz already unzipped from a finished .zip download (the rebuild was interrupted): skip
       straight to rebuilding it. */
    bool readyRvz = false;
    if (file_exists(readyPath)) {
        SplitWriter st;
        staging_writer(e->hash, &st);
        readyRvz = staged_kind(&st) == STAGED_RVZ;
        if (!readyRvz) remove(readyPath);
    }

    /* Network trouble mid-download: wait, reconnect and resume from the saved progress, up to
       MAX_RETRIES times. Each attempt starts from the same resume logic as picking the game again. */
    int rc = 0, closeRc = 0, attempt = 0;
    uint64_t got = 0;
    char err[256] = {0};
    int sumKind = sum_kind(&e->link);
    memset(&gSum, 0, sizeof(gSum));
    transfer_begin();
    if (readyRvz) {
        memset(&s, 0, sizeof(s));
        s.e = e;
        staging_writer(e->hash, &s.sw);
    }
    for (; !readyRvz;) {
    if (attempt > 0 && s.zip) zs_destroy(s.zip);
    memset(&s, 0, sizeof(s));
    s.e = e;
    s.hc.allow7z = true;
    s.guess7z = items && items[e - entries].kind == UI_KIND_7Z;
    staging_writer(e->hash, &s.sw);
    extract_writer(e->hash, &s.convOut);
    wc_init(&s.conv, &s.convOut, statePath);
    s.hc.allowZip = true;
    s.hc.allowRvz = true;
    zip_ckpt_path(e->hash, s.zipCkpt, sizeof(s.zipCkpt));
    gw_init(&s.gw, e->hash, dl_zip_saved, &s);
    snprintf(s.detail, sizeof(s.detail), "%s  -  %s", host, file);
    s.guessRvz = ends_with_ci(file, ".rvz");

    /* Resume an interrupted .zip: its resume point plus what the game writer already has on disk. */
    bool zipResumed = false;
    ZipCkpt *zck = malloc(sizeof(ZipCkpt));
    if (zck && zs_load_ckpt(s.zipCkpt, zck) == 0) {
        bool convert = (zck->userFlag & 1) != 0, rvzIn = (zck->userFlag & 2) != 0, ok = false;
        uint64_t pos = 0;
        if (rvzIn) staging_writer(e->hash, &s.gw.out);
        if (convert) {
            pos = wc_has_state(statePath) ? wc_resume_point(&s.gw.conv) : 0;
            ok = pos > 0 && pos >= zck->outPos;
        } else {
            pos = sw_existing_size(&s.gw.out);
            ok = pos >= zck->outPos;
        }
        if (ok && dl_start_zip(&s) == 0 && zs_resume(s.zip, zck, pos) == 0 && gw_resume(&s.gw, convert, rvzIn, pos) == 0) {
            zipResumed = true;
            s.start = zck->inOffset;
            s.hc.done = true;
            s.hc.isZip = true;
            snprintf(s.zipEntry, sizeof(s.zipEntry), "%s", zck->name);
            if (!rvzIn) sw_remove_all(&s.sw); /* (an .rvz from the zip lives there) */
        } else {
            if (s.zip) zs_destroy(s.zip);
            s.zip = NULL;
            s.zipMode = false;
            gw_discard(&s.gw);
            remove(s.zipCkpt);
            gw_init(&s.gw, e->hash, dl_zip_saved, &s);
        }
    }
    free(zck);

    /* Resume: an interrupted ISO conversion, or raw data that starts like a disc image / .7z. */
    uint64_t convAt = zipResumed ? 0 : wc_has_state(statePath) ? wc_resume_point(&s.conv) : 0;
    if (zipResumed) {
        /* handled above */
    } else if (convAt > 0) {
        s.convert = true;
        s.start = convAt;
        s.hc.done = true;
        s.hc.isWiiIso = true;
        sw_remove_all(&s.sw);
    } else {
        if (wc_has_state(statePath)) wc_discard(&s.conv);
        s.start = sw_existing_size(&s.sw);
        if (s.start >= DISC_HEADER_NEED && staged_kind(&s.sw) != STAGED_BAD) {
            s.hc.done = true;
            s.hc.is7z = staged_kind(&s.sw) == STAGED_7Z;
            s.hc.isRvz = staged_kind(&s.sw) == STAGED_RVZ;
        } else {
            s.start = 0;
        }
    }

    /* checksum: start fresh, carry on, or re-read what a raw download already saved */
    if (sumKind) {
        if (s.start == 0) sum_reset(&gSum, sumKind);
        else if (gSum.valid && gSum.pos == s.start) { /* same session, nothing lost */ }
        else if (!s.zipMode && !s.convert && s.hc.done) {
            sum_reset(&gSum, sumKind);
            if (!sum_prehash(e, s.start)) gSum.valid = false;
        } else {
            gSum.valid = false; /* a converted / unzipped resume: the original bytes are gone */
        }
    }

    UiProgress p0 = {0};
    set_stages(&p0, (s.convert || (s.zipMode && s.gw.convert)) ? MODE_CONVERT
                    : s.guessRvz                                   ? MODE_RVZ
                    : s.guess7z                                    ? MODE_ARCHIVE
                                                                   : MODE_RAW, 0);
    p0.title = e->display[0] ? e->display : e->link.name;
    p0.detail = attempt ? "Reconnecting..." : "Connecting...";
    show_progress(&p0);

    err[0] = '\0';
    DownloadSink sink = {dl_begin, dl_write, dl_progress, &s};
    rc = net_download(cfg, e->link.url, s.start, &sink, err, sizeof(err));

    got = s.zipMode ? zs_in_pos(s.zip) : s.convert ? wc_iso_pos(&s.conv) : sw_existing_size(&s.sw);
    closeRc = 0;
    if (s.zipMode && (rc != 0 || gQuit)) {
        if (s.err[0]) { /* the zip or the game inside is bad / no space: don't resume into it */
            gw_discard(&s.gw);
            remove(s.zipCkpt);
        } else {         /* paused or network trouble: save a resume point and keep everything */
            gw_checkpoint(&s.gw);
            gw_close(&s.gw);
        }
    }
    if (s.badContent) {
        if (s.convert) wc_discard(&s.conv);
        sw_remove_all(&s.sw); /* don't keep (or resume) a web page */
    } else if (s.opened && !s.convert) {
        closeRc = sw_close(&s.sw);
        got = sw_existing_size(&s.sw);
    }
    if (s.opened && s.convert && (rc != 0 || gQuit)) { /* keeps the checkpoint */
        if (!s.err[0]) wc_save(&s.conv);
        wc_close(&s.conv);
    }
    update_sd_text();

    /* everything is closed and resumable here: retry network failures */
    if (rc == -1 && closeRc == 0 && !gQuit && !s.badContent && !s.err[0] && retryable(err)) {
        if (attempt < MAX_RETRIES) {
            attempt++;
            if (retry_wait(e, attempt, err, got, s.expected)) {
                net_reconnect();
                continue;
            }
            rc = 1; /* the user stopped retrying: same as pausing */
        } else {
            snprintf(s.err, sizeof(s.err), "Gave up after %d tries: %s. Your progress is kept; pick the game "
                     "again to continue.", MAX_RETRIES + 1, err);
        }
    }
    break;
    }

    if (gQuit) goto out;
    if (rc == 1) {
        result = 1;
        refresh_entry(e);
        message_box(UI_MSG_INFO, "Download paused", "Your progress is kept. Pick the game again to resume.");
        goto out;
    }
    if (rc != 0 || closeRc != 0) {
        refresh_entry(e);
        message_box(UI_MSG_ERROR, "Download failed",
                    s.err[0] ? s.err : err[0] ? err : "Could not finish writing to the SD card.");
        goto out;
    }

    if (s.expected && got != s.expected) {
        char msg[200], a[32], b[32];
        fmt_size(a, sizeof(a), got);
        fmt_size(b, sizeof(b), s.expected);
        snprintf(msg, sizeof(msg), "Got %s but the server said %s. Pick the game again to retry.", a, b);
        if (s.convert) wc_discard(&s.conv);
        if (s.zipMode) {
            gw_discard(&s.gw);
            remove(s.zipCkpt);
        }
        refresh_entry(e);
        message_box(UI_MSG_ERROR, "Size mismatch", msg);
        goto out;
    }

    /* the whole file arrived: check it against the checksum from links.txt */
    if (sumKind && !readyRvz) {
        if (gSum.valid && gSum.pos == got) {
            char hex[41];
            sum_hex(&gSum, hex);
            if (strcmp(hex, e->link.sum)) {
                char msg[300];
                snprintf(msg, sizeof(msg), "The %s of the download is %s, but links.txt says %s. The file is corrupt "
                         "(or the checksum is wrong), so it was deleted. Pick the game again to download it again.",
                         e->link.sumType, hex, e->link.sum);
                if (s.convert) wc_discard(&s.conv);
                if (s.zipMode) {
                    gw_discard(&s.gw);
                    remove(s.zipCkpt);
                }
                sw_remove_all(&s.sw);
                refresh_entry(e);
                message_box(UI_MSG_ERROR, "Checksum mismatch", msg);
                goto out;
            }
            snprintf(sumNote, sizeof(sumNote), "\nChecksum OK (%s).", e->link.sumType);
        } else {
            snprintf(sumNote, sizeof(sumNote), "\nNot checksum-verified: the download was interrupted and resumed.");
        }
    }

    Mode mode = MODE_RAW;
    SplitWriter game = s.sw; /* what gets moved into place */
    if (readyRvz) {
        mode = MODE_RVZ;
        int cr = convert_rvz(e);
        if (cr != 0) {
            if (cr == 1) result = 1;
            refresh_entry(e);
            goto out;
        }
        extract_writer(e->hash, &game);
    } else if (s.zipMode) {
        mode = s.gw.convert ? MODE_CONVERT : MODE_RAW;
        const char *zerr = NULL;
        if (zs_finish(s.zip) != 0) zerr = zs_error(s.zip);
        else if (gw_finish(&s.gw) != 0) zerr = s.gw.err;
        if (zerr) {
            char msg[256];
            snprintf(msg, sizeof(msg), "%s", zerr);
            gw_discard(&s.gw);
            remove(s.zipCkpt);
            refresh_entry(e);
            message_box(UI_MSG_ERROR, "Unzip failed", msg);
            goto out;
        }
        remove(s.zipCkpt);
        game = s.gw.out;
        if (s.gw.rvz) { /* the zip held an .rvz: it's unzipped into staging, now rebuild the disc */
            mode = MODE_RVZ;
            FILE *rf = fopen(readyPath, "w");
            if (rf) fclose(rf);
            int cr = convert_rvz(e);
            if (cr != 0) {
                if (cr == 1) result = 1;
                refresh_entry(e);
                goto out;
            }
            extract_writer(e->hash, &game);
        }
    } else if (s.convert) {
        mode = MODE_CONVERT;
        UiProgress pc = {0};
        set_stages(&pc, mode, 0);
        pc.title = e->display[0] ? e->display : e->link.name;
        pc.detail = "Writing the WBFS header...";
        pc.done = pc.total = 1;
        show_progress(&pc);
        if (wc_finish(&s.conv) != 0) {
            char msg[256];
            snprintf(msg, sizeof(msg), "%s", s.conv.err);
            wc_discard(&s.conv);
            refresh_entry(e);
            message_box(UI_MSG_ERROR, "Conversion failed", msg);
            goto out;
        }
        game = s.convOut;
    } else if (staged_kind(&s.sw) == STAGED_7Z) {
        mode = MODE_ARCHIVE;
        int xr = extract_archive(e);
        if (xr != 0) {
            if (xr == 1) result = 1;
            refresh_entry(e);
            goto out;
        }
        extract_writer(e->hash, &game);
    } else if (staged_kind(&s.sw) == STAGED_RVZ) {
        mode = MODE_RVZ;
        int cr = convert_rvz(e);
        if (cr != 0) {
            if (cr == 1) result = 1;
            refresh_entry(e);
            goto out;
        }
        extract_writer(e->hash, &game);
    }

    UiProgress pi = {0};
    set_stages(&pi, mode, (mode == MODE_ARCHIVE || mode == MODE_RVZ) ? 2 : 1);
    pi.title = e->display[0] ? e->display : e->link.name;
    pi.detail = "Moving files into place...";
    pi.done = pi.total = 1;
    show_progress(&pi);

    char ferr[256] = {0};
    if (finalize(e, &game, ferr, sizeof(ferr)) != 0) {
        sw_remove_all(&game);
        refresh_entry(e);
        message_box(UI_MSG_ERROR, "Install failed", ferr);
        goto out;
    }
    /* clean up: the .7z (and the .txt inside it, which was never extracted) */
    if (mode == MODE_ARCHIVE || mode == MODE_RVZ) sw_remove_all(&s.sw); /* the archive / .rvz is done with */
    remove(statePath);
    remove(readyPath);
    rmdir(stage);
    snprintf(e->discId, sizeof(e->discId), "%s", e->id);
    {
        int m = 0, u = 0;
        covers_score(e->discId, &m, &u);
        e->metacritic = m;
    }
    refresh_status(e);
    e->coverState = covers_have(e->discId) ? COVER_CACHED : COVER_UNKNOWN; /* fetch the art now that the ID is known */
    gWinCenter = -100000;
    rebuild_items();
    update_sd_text();
    if (s.zip) zs_destroy(s.zip);
    s.zip = NULL;
    transfer_end();

    char msg[460];
    snprintf(msg, sizeof(msg), "Saved to %s/%s/%s%s\nLaunch it with %s.%s", drive_prefix(e->usb), e->gc ? "games" : "wbfs",
             e->folder, e->disc ? "/disc2.iso" : "", e->gc ? "Nintendont" : "USB Loader GX or WiiFlow", sumNote);
    if (!gBatch) chime();
    message_box(UI_MSG_OK, "Installed!", msg);
    return 0;

out:
    if (s.zip) zs_destroy(s.zip);
    s.zip = NULL;
    transfer_end();
    return result;
}

/* ------------------------------------------------------------------ manage games */

static void delete_installed(Entry *e)
{
    SplitWriter sw;
    final_writer(e->usb, e->gc, e->disc, e->folder, e->id, &sw);
    sw_remove_all(&sw);
    char dir[600];
    snprintf(dir, sizeof(dir), "%s", sw.base);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        rmdir(dir); /* only goes if empty: covers or saves the loader put there stay */
    }
    e->folder[0] = '\0';
    e->id[0] = '\0';
    save_installed();
    refresh_status(e);
    rebuild_items();
    update_sd_text();
}

static void discard_partial(Entry *e)
{
    SplitWriter sw;
    staging_writer(e->hash, &sw);
    sw_remove_all(&sw);
    extract_writer(e->hash, &sw);
    sw_remove_all(&sw);
    static const char *files[] = {"convert.state", "extract.state", "rvz.state", "zip.ckpt", "rvz.ready"};
    char path[400];
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s/%s", gTmpDir, e->hash, files[i]);
        remove(path);
    }
    snprintf(path, sizeof(path), "%s/%s", gTmpDir, e->hash);
    rmdir(path);
    refresh_status(e);
    rebuild_items();
    update_sd_text();
}

static uint64_t partial_bytes(Entry *e)
{
    SplitWriter sw;
    staging_writer(e->hash, &sw);
    uint64_t n = sw_existing_size(&sw);
    extract_writer(e->hash, &sw);
    return n + sw_existing_size(&sw);
}

/* file size via a HEAD request, on its own thread so the details screen stays responsive */
typedef struct {
    char url[1024];
    Config cfg;
    SDL_atomic_t state; /* 0 running, 1 done, 2 nobody waiting any more */
    uint64_t size;
} SizeJob;

static int size_worker(void *ud)
{
    SizeJob *j = ud;
    j->size = net_fetch_size(&j->cfg, j->url);
    if (!SDL_AtomicCAS(&j->state, 0, 1)) free(j); /* abandoned */
    return 0;
}

static void size_abandon(SizeJob *j)
{
    if (j && !SDL_AtomicCAS(&j->state, 0, 2)) free(j); /* already finished */
}

static bool gNetOk;

static int run_one(const Config *cfg, int idx); /* below */

static void details_screen(const Config *cfg, int idx)
{
    Entry *e = &entries[idx];
    SizeJob *job = NULL;
    if (gNetOk && !e->installed && !e->remoteSize) {
        job = calloc(1, sizeof(SizeJob));
        if (job) {
            snprintf(job->url, sizeof(job->url), "%s", e->link.url);
            job->cfg = *cfg;
            SDL_Thread *t = SDL_CreateThread(size_worker, "size", job);
            if (t) SDL_DetachThread(t);
            else {
                free(job);
                job = NULL;
            }
        }
    }
    uint64_t have = e->partial ? partial_bytes(e) : 0;
    bool confirm = false;
    while (!gQuit) {
        pump();
        cover_window_update();
        if (job && SDL_AtomicGet(&job->state) == 1) {
            e->remoteSize = job->size;
            free(job);
            job = NULL;
        }
        Input in = read_input();
        if (confirm) {
            if (in.pressed & VPAD_BUTTON_A) {
                if (e->installed) delete_installed(e);
                else discard_partial(e);
                have = 0;
                confirm = false;
            } else if (in.pressed & VPAD_BUTTON_B) {
                confirm = false;
            }
        } else {
            if (in.pressed & VPAD_BUTTON_B) break;
            if (((in.pressed & VPAD_BUTTON_A) || in.tap) && gNetOk) {
                run_one(cfg, idx);
                have = e->partial ? partial_bytes(e) : 0;
                continue;
            }
            if (in.pressed & VPAD_BUTTON_Y) {
                queue_toggle(idx);
                rebuild_items();
            }
            if (in.pressed & VPAD_BUTTON_X) {
                e->fav = !e->fav;
                save_hash_list(FAV_PATH, true);
                fill_item(idx);
            }
            if ((in.pressed & VPAD_BUTTON_MINUS) && (e->installed || e->partial)) confirm = true;
        }
        if (!gVisible) continue;

        char idText[200], typeText[60], regionText[40], sizeText[48], statusText[200], src[400], sumText[80];
        char host[128], file[200];
        url_parts(e->link.url, host, sizeof(host), file, sizeof(file));
        if (e->discId[0]) snprintf(idText, sizeof(idText), "%s", e->discId);
        else snprintf(idText, sizeof(idText), "Not known yet%s%s", e->coverWhy[0] ? ": " : "", e->coverWhy);
        UiKind k = items ? items[idx].kind : UI_KIND_UNKNOWN;
        const char *disc = e->installed ? (e->gc ? "GameCube" : "Wii")
                           : e->discId[0] ? ((e->discId[0] == 'G' || e->discId[0] == 'D') ? "GameCube" : "Wii")
                                          : "Wii or GameCube";
        const char *wrap = ends_with_ci(file, ".zip") ? " (in a .zip)" : ends_with_ci(file, ".7z") ? " (in a .7z)"
                           : ends_with_ci(file, ".rvz") ? " (.rvz, rebuilt on install)"
                           : ends_with_ci(file, ".iso") && !(e->discId[0] == 'G' || e->discId[0] == 'D') ? " (.iso, converted to WBFS)"
                                                                                                          : "";
        snprintf(typeText, sizeof(typeText), "%s%s", disc, wrap);
        regions_text(entry_regions(e), regionText, sizeof(regionText));
        if (e->remoteSize) fmt_size(sizeText, sizeof(sizeText), e->remoteSize);
        else snprintf(sizeText, sizeof(sizeText), "%s", job ? "Checking..." : "Unknown");
        if (e->installed)
            snprintf(statusText, sizeof(statusText), "%s/%s/%s", drive_prefix(e->usb), e->gc ? "games" : "wbfs", e->folder);
        else if (e->partial) {
            char a[24];
            fmt_size(a, sizeof(a), have);
            snprintf(statusText, sizeof(statusText), "%s saved, A to resume", a);
        } else snprintf(statusText, sizeof(statusText), "Not downloaded");
        snprintf(src, sizeof(src), "%s  -  %s", host, file);
        if (e->link.sumType[0]) snprintf(sumText, sizeof(sumText), "%s, checked after download", e->link.sumType);
        char metaText[80] = "";
        if (e->metacritic > 0) snprintf(metaText, sizeof(metaText), "%d / 100  -  via RAWG.io", e->metacritic);

        UiDetails d;
        memset(&d, 0, sizeof(d));
        d.title = e->display[0] ? e->display : e->link.name;
        d.cover = e->cover;
        d.kind = k;
        d.status = e->installed ? UI_ST_INSTALLED : e->partial ? UI_ST_PARTIAL : UI_ST_NEW;
        d.fav = e->fav;
        d.queuePos = queue_pos(idx);
        d.metacritic = e->metacritic;
        const char *rows[][2] = {{"Game ID", idText},     {"Type", typeText},       {"Region", regionText},
                                 {"Metacritic", metaText}, {"Size", sizeText},       {"Location", statusText},
                                 {"Source", src},          {"Checksum", sumText}};
        int nr = 0;
        for (int i = 0; i < 8; i++) {
            if (i == 3 && !metaText[0]) continue;      /* no key: no Metacritic row */
            if (i == 7 && !e->link.sumType[0]) continue; /* no checksum given */
            d.rows[nr][0] = rows[i][0];
            d.rows[nr][1] = rows[i][1];
            nr++;
        }
        d.rowCount = nr;

        ui_begin_frame(now_s());
        ui_header(gSdText, gMusicState);
        ui_details(&d);
        UiHint hints[5];
        int nh = 0;
        hints[nh++] = (UiHint){"A", e->installed ? "Reinstall" : e->partial ? "Resume" : "Install"};
        hints[nh++] = (UiHint){"Y", d.queuePos ? "Unqueue" : "Queue"};
        hints[nh++] = (UiHint){"X", e->fav ? "Unfavorite" : "Favorite"};
        if (e->installed) hints[nh++] = (UiHint){"-", "Delete"};
        else if (e->partial) hints[nh++] = (UiHint){"-", "Discard"};
        hints[nh++] = (UiHint){"B", "Back"};
        ui_footer(hints, nh, NULL);
        if (confirm) {
            char body[300];
            if (e->installed)
                snprintf(body, sizeof(body), "This deletes %s/%s/%s from the %s. You can download it again later.",
                         drive_prefix(e->usb), e->gc ? "games" : "wbfs", e->folder, e->usb ? "USB drive" : "SD card");
            else
                snprintf(body, sizeof(body), "This deletes the partly downloaded files. The download starts over next time.");
            ui_confirm(e->installed ? "Delete this game?" : "Discard the download?", body,
                       e->installed ? "Delete" : "Discard");
        }
        ui_end_frame();
    }
    size_abandon(job);
}

/* one game from the list or the details screen; drops it from the queue once installed */
static int run_one(const Config *cfg, int idx)
{
    int r = download_entry(cfg, &entries[idx]);
    if (r == 0 && queue_pos(idx)) {
        queue_toggle(idx);
        rebuild_items();
    }
    return r;
}

/* move a queued game one place earlier (dir < 0) or later */
static void queue_move(int idx, int dir)
{
    int pos = queue_pos(idx) - 1;
    int to = pos + (dir < 0 ? -1 : 1);
    if (pos < 0 || to < 0 || to >= gQueueCount) return;
    int t = gQueue[to];
    gQueue[to] = gQueue[pos];
    gQueue[pos] = t;
    save_hash_list(QUEUE_PATH, false);
}

/* "about 1 h 20 min" */
static void fmt_eta(char *out, size_t n, uint64_t seconds)
{
    if (seconds < 60) snprintf(out, n, "less than a minute");
    else if (seconds < 3600) snprintf(out, n, "about %u min", (unsigned)((seconds + 30) / 60));
    else snprintf(out, n, "about %u h %u min", (unsigned)(seconds / 3600), (unsigned)(seconds % 3600 / 60));
}

/* queue progress for the note line: game n of total, and the time left for the whole queue */
static int gBatchIndex, gBatchTotal;
static uint64_t gBatchAfter;   /* bytes of the games after this one (0 if any size is unknown) */
static bool gBatchSizesKnown;

static void batch_note(uint64_t done, uint64_t total, uint64_t bps)
{
    char eta[64] = "";
    if (gBatchSizesKnown && total > done && bps > 0) {
        char t[40];
        fmt_eta(t, sizeof(t), (total - done + gBatchAfter) / bps);
        snprintf(eta, sizeof(eta), "  -  %s left", t);
    }
    snprintf(gBatchNote, sizeof(gBatchNote), "Queue: game %d of %d%s  -  B pauses the queue", gBatchIndex, gBatchTotal, eta);
}

/* a soft two-note chime, generated once (no sound file) */
static Mix_Chunk *gChime;
static int16_t *gChimePcm;

static void chime_init(void)
{
    int freq = 0, ch = 0;
    Uint16 fmt = 0;
    if (!Mix_QuerySpec(&freq, &fmt, &ch) || fmt != AUDIO_S16SYS || ch != 2) return;
    int frames = freq * 3 / 2;
    gChimePcm = calloc((size_t)frames * 2, sizeof(int16_t));
    if (!gChimePcm) return;
    static const float notes[2] = {880.0f, 1174.66f}; /* A5, then D6 */
    for (int k = 0; k < 2; k++) {
        int start = k * freq / 5;
        for (int i = start; i < frames; i++) {
            float t = (float)(i - start) / freq;
            float env = expf(-t * 3.2f) * (t < 0.004f ? t / 0.004f : 1.0f);
            float s = (sinf(2 * (float)M_PI * notes[k] * t) + 0.3f * sinf(2 * (float)M_PI * notes[k] * 2.76f * t) *
                       expf(-t * 6.0f)) * env * 0.22f;
            int v = gChimePcm[i * 2] + (int)(s * 32767);
            v = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
            gChimePcm[i * 2] = gChimePcm[i * 2 + 1] = (int16_t)v;
        }
    }
    gChime = Mix_QuickLoad_RAW((Uint8 *)gChimePcm, (Uint32)(frames * 4));
}

static void chime(void)
{
    if (gChimeOn && gChime) Mix_PlayChannel(-1, gChime, 0);
}

/* "Turning off in 15 s" with B to cancel. Returns true if the console should turn off. */
static bool power_off_countdown(void)
{
    double until = now_s() + 15;
    while (!gQuit) {
        pump();
        if (read_input().pressed & (VPAD_BUTTON_B | VPAD_BUTTON_A)) return false;
        double left = until - now_s();
        if (left <= 0) return true;
        char body[160];
        snprintf(body, sizeof(body), "The queue is done. Turning the Wii U off in %d s.\nPress B to keep it on.",
                 (int)left + 1);
        if (gVisible) {
            draw_list_scene();
            ui_message(UI_MSG_INFO, "Turning off", body, "Keep it on (B)");
            ui_end_frame();
        }
    }
    return false;
}

static void run_queue(const Config *cfg)
{
    int total = gQueueCount, done = 0, failed = 0, n = 0;
    bool paused = false;
    char failNames[300] = "", firstErr[400] = "";

    /* sizes of the queued games, for "time left" (a quick question to the server for each) */
    gBatchSizesKnown = true;
    for (int i = 0; i < gQueueCount && !gQuit; i++) {
        Entry *q = &entries[gQueue[i]];
        if (!q->remoteSize) {
            UiProgress p = {0};
            p.stages[0] = "Preparing";
            p.stageCount = 1;
            p.title = "Getting the queue ready";
            p.detail = q->display[0] ? q->display : q->link.name;
            p.done = (uint64_t)i;
            p.total = (uint64_t)gQueueCount;
            pump();
            show_progress(&p);
            q->remoteSize = net_fetch_size(cfg, q->link.url);
        }
        if (!q->remoteSize) gBatchSizesKnown = false;
    }

    gBatch = true;
    gBatchTotal = total;
    while (gQueueCount > 0 && !gQuit) {
        int idx = gQueue[0];
        n++;
        gBatchIndex = n;
        gBatchAfter = 0;
        for (int i = 1; i < gQueueCount; i++) gBatchAfter += entries[gQueue[i]].remoteSize;
        batch_note(0, 0, 0);
        gBatchErr[0] = '\0';
        int r = download_entry(cfg, &entries[idx]);
        if (r == 1 || gQuit) {
            paused = true;
            break;
        }
        queue_toggle(idx);
        if (r == 0) done++;
        else {
            failed++;
            size_t have = strlen(failNames);
            snprintf(failNames + have, sizeof(failNames) - have, "%s%s", have ? ", " : "",
                     entries[idx].display[0] ? entries[idx].display : entries[idx].link.name);
            if (!firstErr[0]) snprintf(firstErr, sizeof(firstErr), "%s", gBatchErr);
        }
        rebuild_items();
    }
    gBatch = false;
    gBatchNote[0] = '\0';
    rebuild_items();
    build_view();
    if (gQuit) return;
    char msg[1100];
    int m = snprintf(msg, sizeof(msg), "Installed %d of %d.", done, total);
    if (failed) m += snprintf(msg + m, sizeof(msg) - m, "\nFailed (%d): %s\n%s", failed, failNames, firstErr);
    if (paused) snprintf(msg + m, sizeof(msg) - m, "\nPaused with %d left in the queue. Choose Download the queue in "
                                                  "the + menu to carry on.", gQueueCount);
    if (!paused) chime();
    if (!paused && gQueueOff && power_off_countdown()) {
        save_state();
        OSShutdown();
        return;
    }
    message_box(failed ? UI_MSG_ERROR : paused ? UI_MSG_INFO : UI_MSG_OK,
                paused ? "Queue paused" : failed ? "Queue finished, with problems" : "Queue finished", msg);
}

/* ------------------------------------------------------------------ SD card manager */

/* Every installed game on the SD card, whoever put it there: sd:/wbfs/<folder>/<ID>.wbfs (+ .wbf1...)
   and sd:/games/<folder>/game.iso (+ disc2.iso). */
typedef struct {
    char folder[256];
    char id[7];
    char title[128];
    char subtitle[200];
    bool gc;
    bool usb;
    int discs;
    uint64_t bytes;
    SDL_Texture *cover;
} SdGame;

static SdGame *gSd;
static int gSdCount;

static bool is_wbfs_part(const char *name)
{
    size_t n = strlen(name);
    if (n < 6) return false;
    const char *ext = strrchr(name, '.');
    if (!ext) return false;
    if (!strcasecmp(ext, ".wbfs")) return true;
    return strlen(ext) == 5 && !strncasecmp(ext, ".wbf", 4) && ext[4] >= '1' && ext[4] <= '9';
}

static bool is_gc_file(const char *name)
{
    return !strcasecmp(name, "game.iso") || !strcasecmp(name, "disc2.iso") || !strcasecmp(name, "game.ciso") ||
           !strcasecmp(name, "game.gcm");
}

static uint64_t file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (uint64_t)st.st_size : 0;
}

/* "Mario Kart Wii [RMCE01]" -> title "Mario Kart Wii", ID "RMCE01" */
static void split_folder(const char *folder, char *title, size_t tn, char id[7])
{
    snprintf(title, tn, "%.127s", folder);
    id[0] = '\0';
    char *b = strrchr(title, '[');
    if (b && strlen(b) == 8 && b[7] == ']' && valid_id(b + 1)) {
        snprintf(id, 7, "%.6s", b + 1);
        *b = '\0';
        size_t n = strlen(title);
        while (n > 0 && title[n - 1] == ' ') title[--n] = '\0';
        if (!title[0]) snprintf(title, tn, "%s", id);
    }
}

static int cmp_sd(const void *a, const void *b)
{
    return strcasecmp(((const SdGame *)a)->title, ((const SdGame *)b)->title);
}

static void sd_free(void)
{
    for (int i = 0; i < gSdCount; i++)
        if (gSd[i].cover) SDL_DestroyTexture(gSd[i].cover);
    free(gSd);
    gSd = NULL;
    gSdCount = 0;
}

static void sd_scan(void)
{
    sd_free();
    int cap = 0;
    for (int pass = 0; pass < 4; pass++) {
        bool gc = pass & 1, usb = pass >= 2;
        if (usb && !usbfs_mounted()) break;
        char root[300];
        snprintf(root, sizeof(root), "%s/%s", game_root(usb), gc ? "games" : "wbfs");
        DIR *d = opendir(root);
        if (!d) continue;
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.') continue;
            char dir[600];
            snprintf(dir, sizeof(dir), "%s/%s", root, de->d_name);
            DIR *g = opendir(dir);
            if (!g) continue; /* a file, not a game folder */
            SdGame s;
            memset(&s, 0, sizeof(s));
            s.gc = gc;
            s.usb = usb;
            snprintf(s.folder, sizeof(s.folder), "%s", de->d_name);
            split_folder(de->d_name, s.title, sizeof(s.title), s.id);
            struct dirent *fe;
            int files = 0;
            while ((fe = readdir(g)) != NULL) {
                if (!(gc ? is_gc_file(fe->d_name) : is_wbfs_part(fe->d_name))) continue;
                char path[900];
                snprintf(path, sizeof(path), "%s/%s", dir, fe->d_name);
                s.bytes += file_size(path);
                files++;
                if (gc && !strcasecmp(fe->d_name, "disc2.iso")) s.discs = 2;
                if (!gc && !s.id[0] && strlen(fe->d_name) == 11 && !strcasecmp(fe->d_name + 6, ".wbfs") &&
                    valid_id(fe->d_name))
                    snprintf(s.id, sizeof(s.id), "%.6s", fe->d_name);
            }
            closedir(g);
            if (!files) continue; /* not a game folder (or an empty one) */
            if (gc && !s.id[0]) { /* GameCube folders without "[ID]": read it from the disc */
                char path[700];
                snprintf(path, sizeof(path), "%s/game.iso", dir);
                DiscInfo info;
                if (disc_identify_file(path, &info) != DISC_UNKNOWN) snprintf(s.id, sizeof(s.id), "%s", info.id);
            }
            char sz[24];
            fmt_size(sz, sizeof(sz), s.bytes);
            snprintf(s.subtitle, sizeof(s.subtitle), "%s%s%s  -  %s%s  -  %s", s.id, s.id[0] ? "  -  " : "",
                     gc ? "GameCube" : "Wii", sz, s.discs == 2 ? "  -  2 discs" : "", usb ? "USB drive" : "SD card");
            if (gSdCount == cap) {
                cap = cap ? cap * 2 : 32;
                SdGame *n = realloc(gSd, (size_t)cap * sizeof(SdGame));
                if (!n) break;
                gSd = n;
            }
            gSd[gSdCount++] = s;
        }
        closedir(d);
    }
    if (gSdCount > 1) qsort(gSd, (size_t)gSdCount, sizeof(SdGame), cmp_sd);
}

/* delete one game's files (only game files: covers or saves a loader put in the folder stay) */
static void sd_delete(const SdGame *s)
{
    char dir[600];
    snprintf(dir, sizeof(dir), "%s/%s/%s", game_root(s->usb), s->gc ? "games" : "wbfs", s->folder);
    DIR *g = opendir(dir);
    if (g) {
        struct dirent *fe;
        char names[16][256];
        int n = 0;
        while ((fe = readdir(g)) != NULL && n < 16)
            if (s->gc ? is_gc_file(fe->d_name) : is_wbfs_part(fe->d_name)) snprintf(names[n++], 256, "%s", fe->d_name);
        closedir(g);
        for (int i = 0; i < n; i++) {
            char path[900];
            snprintf(path, sizeof(path), "%s/%.255s", dir, names[i]);
            remove(path);
        }
    }
    rmdir(dir); /* only goes if nothing else is left in it */
    /* links that pointed at it aren't installed any more */
    bool changed = false;
    for (int i = 0; i < entryCount; i++) {
        Entry *e = &entries[i];
        if (e->folder[0] && e->gc == s->gc && e->usb == s->usb && !strcmp(e->folder, s->folder)) {
            e->folder[0] = '\0';
            e->id[0] = '\0';
            refresh_status(e);
            changed = true;
        }
    }
    if (changed) {
        save_installed();
        rebuild_items();
    }
    update_sd_text();
}

static void storage_screen(void)
{
    sd_scan();
    int sel = 0;
    bool confirm = false;
    UiItem *list = NULL;
    while (!gQuit) {
        pump();
        Input in = read_input();
        if (confirm) {
            if (in.pressed & VPAD_BUTTON_A) {
                sd_delete(&gSd[sel]);
                sd_scan();
                if (sel >= gSdCount) sel = gSdCount - 1;
                if (sel < 0) sel = 0;
                confirm = false;
            } else if (in.pressed & VPAD_BUTTON_B) {
                confirm = false;
            }
        } else {
            if (in.pressed & (VPAD_BUTTON_B | VPAD_BUTTON_PLUS)) break;
            if (gSdCount > 0) {
                if (in.pressed & VPAD_BUTTON_RIGHT) sel++;
                if (in.pressed & VPAD_BUTTON_LEFT) sel--;
                if (in.pressed & VPAD_BUTTON_DOWN) sel += 5;
                if (in.pressed & VPAD_BUTTON_UP) sel -= 5;
                if (sel >= gSdCount) sel = gSdCount - 1;
                if (sel < 0) sel = 0;
                if (in.tap) {
                    int hit = ui_hit_item(in.tapX, in.tapY, gSdCount);
                    if (hit >= 0) sel = hit;
                }
                if (in.pressed & (VPAD_BUTTON_MINUS | VPAD_BUTTON_A)) confirm = true;
            }
        }
        if (!gVisible) continue;

        /* covers for the games near the selection (from the cover cache only) */
        for (int i = 0; i < gSdCount; i++) {
            SdGame *s = &gSd[i];
            bool near = abs(i - sel) <= 6;
            if (near && !s->cover && s->id[0] && covers_have(s->id)) {
                char path[300];
                covers_path(s->id, path, sizeof(path));
                SDL_Surface *surf = IMG_Load(path);
                if (surf) {
                    s->cover = SDL_CreateTextureFromSurface(gRenderer, surf);
                    SDL_FreeSurface(surf);
                }
            } else if (!near && s->cover && abs(i - sel) > 20) {
                SDL_DestroyTexture(s->cover);
                s->cover = NULL;
            }
        }
        free(list);
        list = gSdCount ? calloc((size_t)gSdCount, sizeof(UiItem)) : NULL;
        uint64_t used = 0;
        for (int i = 0; i < gSdCount && list; i++) {
            list[i].title = gSd[i].title;
            list[i].subtitle = gSd[i].subtitle;
            list[i].kind = gSd[i].gc ? UI_KIND_GC : UI_KIND_WII;
            list[i].status = UI_ST_INSTALLED;
            list[i].cover = gSd[i].cover;
            used += gSd[i].bytes;
        }
        ui_begin_frame(now_s());
        ui_header(gSdText, gMusicState);
        if (gSdCount == 0) {
            ui_empty("No games installed", usbfs_mounted() ? "Nothing in /wbfs or /games on the SD card or the USB drive yet."
                                                           : "Nothing in sd:/wbfs or sd:/games yet.", NULL);
            UiHint h[1] = {{"B", "Back"}};
            ui_footer(h, 1, NULL);
        } else {
            ui_list(list, gSdCount, sel);
            char right[64], u[24];
            fmt_size(u, sizeof(u), used);
            snprintf(right, sizeof(right), "%d game%s, %s    %d / %d", gSdCount, gSdCount == 1 ? "" : "s", u, sel + 1,
                     gSdCount);
            UiHint h[2] = {{"-", "Delete"}, {"B", "Back"}};
            ui_footer(h, 2, right);
        }
        if (confirm && gSdCount > 0) {
            char body[700], sz[24];
            fmt_size(sz, sizeof(sz), gSd[sel].bytes);
            snprintf(body, sizeof(body), "This deletes %s (%s) from %s/%s/%s. It can't be undone.", gSd[sel].title, sz,
                     drive_prefix(gSd[sel].usb), gSd[sel].gc ? "games" : "wbfs", gSd[sel].folder);
            ui_confirm("Delete this game?", body, "Delete");
        }
        ui_end_frame();
    }
    free(list);
    sd_free();
}

/* Point installs (and staging) at the SD card or the USB drive. Returns false (with a message) if the USB
   drive can't be used. */
static bool set_target(bool usb, bool quiet)
{
    if (usb && !usbfs_mounted()) {
        char err[300];
        if (usbfs_mount(err, sizeof(err)) != 0) {
            if (!quiet) message_box(UI_MSG_ERROR, "Can't use the USB drive", err);
            else snprintf(gStartNote, sizeof(gStartNote), "Couldn't use the USB drive, so games go to the SD card. %.200s", err);
            usb = false;
        }
    }
    if (usbfs_mounted()) gUsbSeen = true;
    gUseUsb = usb;
    snprintf(gTmpDir, sizeof(gTmpDir), "%s", usb ? USB_TMP : TMP_DIR);
    mkdir_p(gTmpDir);
    update_sd_text();
    /* "partly downloaded" depends on which drive's staging folder we look in */
    if (entryCount) {
        scan_tmp_dir();
        for (int i = 0; i < entryCount; i++) refresh_status(&entries[i]);
        gTmpSetValid = false;
        rebuild_items();
    }
    return gUseUsb == usb;
}

/* + menu. Returns MENU_* */
enum { MENU_NONE, MENU_EXIT, MENU_QUEUE, MENU_RELOAD, MENU_STORAGE, MENU_UPDATE };

static void reapply_view(void)
{
    int keep = cur_entry();
    build_view();
    gSel = (keep >= 0 && gPosOf && gPosOf[keep] >= 0) ? gPosOf[keep] : 0;
    gWinCenter = -100000;
    save_state();
}

static int menu_screen(void)
{
    static const char *sorts[] = {"List order", "A-Z", "Installed first", "Metacritic"};
    static const char *shows[] = {"All games", "Favorites", "Installed", "Not installed", "Queued"};
    enum { M_QUEUE, M_CLEAR, M_QOFF, M_TARGET, M_SORT, M_SHOW, M_ONLY, M_DUPES, M_REGION, M_SD, M_MUSIC, M_CHIME, M_RELOAD,
           M_UPDATE, M_EXIT, M_N };
    int sel = gQueueCount ? M_QUEUE : M_SORT;
    while (!gQuit) {
        pump();
        Input in = read_input();
        char qv[32];
        snprintf(qv, sizeof(qv), "%d game%s", gQueueCount, gQueueCount == 1 ? "" : "s");
        UiMenuItem it[M_N] = {
            {"Download the queue", qv, false, gQueueCount == 0},
            {"Clear the queue", NULL, true, gQueueCount == 0},
            {"When the queue is done", gQueueOff ? "Turn off the Wii U" : "Stay on", false, false},
            {"Install to", gUseUsb ? "USB drive (FAT32)" : "SD card", false, false},
            {"Sort", sorts[gSort], false, false},
            {"Show", shows[gShow], false, false},
            {"Regions", gRegionOnly == 'E' ? "USA only" : gRegionOnly == 'P' ? "Europe only" : gRegionOnly == 'J' ? "Japan only"
                                                                                                          : "All regions", false, false},
            {"Region duplicates", gHideDupes ? "Hidden" : "Shown", false, false},
            {"Preferred region", gRegionPref == 'P' ? "Europe" : gRegionPref == 'J' ? "Japan" : "USA", false, false},
            {"Installed games", gSdText[0] ? gSdText : NULL, false, false},
            {"Music", gMusicState == 1 ? "On" : "Off", false, gMusicState < 0},
            {"Sounds", gChimeOn ? "On" : "Off", false, !gChime},
            {"Reload the list", NULL, false, false},
            {"Check for updates", NULL, false, !gNetOk},
            {"Exit WiiDrop", NULL, true, false},
        };
        int step = 0;
        bool activate = false;
        if (in.pressed & (VPAD_BUTTON_B | VPAD_BUTTON_PLUS)) return MENU_NONE;
        if (in.pressed & VPAD_BUTTON_DOWN) sel = (sel + 1) % M_N;
        if (in.pressed & VPAD_BUTTON_UP) sel = (sel + M_N - 1) % M_N;
        if (in.pressed & VPAD_BUTTON_RIGHT) step = 1;
        if (in.pressed & VPAD_BUTTON_LEFT) step = -1;
        if (in.pressed & VPAD_BUTTON_A) activate = true;
        if (in.tap) {
            int hit = ui_menu_hit(in.tapX, in.tapY, M_N);
            if (hit >= 0) {
                sel = hit;
                activate = true;
            }
        }
        if (activate) step = 1;
        if (step && !it[sel].disabled) {
            switch (sel) {
            case M_QUEUE: if (activate) return MENU_QUEUE; break;
            case M_CLEAR:
                if (activate) {
                    gQueueCount = 0;
                    save_hash_list(QUEUE_PATH, false);
                    rebuild_items();
                    reapply_view();
                }
                break;
            case M_SORT: gSort = (gSort + SORT_COUNT + step) % SORT_COUNT; reapply_view(); break;
            case M_SHOW: gShow = (gShow + SHOW_COUNT + step) % SHOW_COUNT; reapply_view(); break;
            case M_ONLY: {
                const char *order = "\0EPJ";
                int k = gRegionOnly ? (int)(strchr(order + 1, gRegionOnly) - order) : 0;
                gRegionOnly = order[(k + 4 + step) % 4];
                if (gRegionOnly) gRegionPref = gRegionOnly; /* duplicates: keep the same region */
                reapply_view();
                break;
            }
            case M_DUPES: gHideDupes = !gHideDupes; reapply_view(); break;
            case M_REGION: {
                const char *order = "EPJ";
                int k = (int)(strchr(order, gRegionPref) - order);
                gRegionPref = order[(k + 3 + step) % 3];
                reapply_view();
                break;
            }
            case M_MUSIC: music_toggle(); save_state(); break;
            case M_QOFF: gQueueOff = !gQueueOff; save_state(); break;
            case M_TARGET:
                set_target(!gUseUsb, false);
                save_state();
                break;
            case M_CHIME:
                gChimeOn = !gChimeOn;
                save_state();
                chime();
                break;
            case M_RELOAD: if (activate) return MENU_RELOAD; break;
            case M_SD: if (activate) return MENU_STORAGE; break;
            case M_UPDATE: if (activate) return MENU_UPDATE; break;
            case M_EXIT: if (activate) return MENU_EXIT; break;
            }
        }
        if (!gVisible) continue;
        draw_list_scene();
        ui_menu("WiiDrop " WIIDROP_VERSION, it, M_N, sel);
        ui_end_frame();
    }
    return MENU_NONE;
}

/* ------------------------------------------------------------------ self-update */

/* New versions come from this GitHub repository's releases (it has to be public). */
#define UPDATE_REPO "Jasifryx/wiidrop"

typedef struct {
    char tag[40];
    char url[600];
    char sha256[65]; /* lowercase hex, "" if GitHub didn't give one */
    uint64_t size;
} UpdateInfo;

static UpdateInfo gUpd;
static SDL_atomic_t gUpdState; /* 0 not checked, 1 checking, 2 a newer version is available, 3 nothing, 4 failed */
static char gUpdErr[256];
static Config gUpdCfg;

/* "v1.0.0-beta.3" -> {1, 0, 0, rank, n}: rank 9 = final, 3 = rc, 2 = beta, 1 = alpha */
static void parse_version(const char *v, int out[5])
{
    memset(out, 0, 5 * sizeof(int));
    if (*v == 'v' || *v == 'V') v++;
    sscanf(v, "%d.%d.%d", &out[0], &out[1], &out[2]);
    const char *dash = strchr(v, '-');
    out[3] = 9;
    if (dash) {
        dash++;
        out[3] = !strncasecmp(dash, "rc", 2) ? 3 : !strncasecmp(dash, "beta", 4) ? 2 : !strncasecmp(dash, "alpha", 5) ? 1 : 0;
        const char *num = dash + strcspn(dash, "0123456789");
        out[4] = atoi(num);
    }
}

static int version_cmp(const char *a, const char *b)
{
    int x[5], y[5];
    parse_version(a, x);
    parse_version(b, y);
    for (int i = 0; i < 5; i++)
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}

/* Value of "key" (a string or a bare literal) in JSON text between p and end. Returns where it ended, or NULL. */
static const char *json_value(const char *p, const char *end, const char *key, char *out, size_t n)
{
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    size_t pl = strlen(pat);
    for (; p && p + pl < end; p++) {
        if (memcmp(p, pat, pl)) continue;
        const char *q = p + pl;
        while (q < end && (*q == ' ' || *q == '\n' || *q == '\r' || *q == '\t')) q++;
        if (q >= end || *q != ':') continue;
        q++;
        while (q < end && (*q == ' ' || *q == '\n' || *q == '\r' || *q == '\t')) q++;
        size_t j = 0;
        if (q < end && *q == '"') {
            for (q++; q < end && *q != '"'; q++) {
                if (*q == '\\' && q + 1 < end) q++;
                if (j + 1 < n) out[j++] = *q;
            }
        } else {
            for (; q < end && *q != ',' && *q != '}' && *q != ']' && *q != ' '; q++)
                if (j + 1 < n) out[j++] = *q;
        }
        out[j] = '\0';
        return q;
    }
    return NULL;
}

static int update_worker(void *ud)
{
    (void)ud;
    char url[200], err[256];
    snprintf(url, sizeof(url), "https://api.github.com/repos/%s/releases?per_page=20", UPDATE_REPO);
    char *json = net_fetch_text(&gUpdCfg, url, err, sizeof(err));
    int found = 0;
    if (json) {
        int cur[5];
        parse_version(WIIDROP_VERSION, cur);
        bool allowPre = cur[3] != 9; /* betas update to newer betas; finals only to finals */
        const char *end = json + strlen(json);
        UpdateInfo best;
        memset(&best, 0, sizeof(best));
        snprintf(best.tag, sizeof(best.tag), "%s", WIIDROP_VERSION);
        /* one release per "tag_name"; its prerelease flag and assets follow it */
        const char *p = strstr(json, "\"tag_name\"");
        while (p) {
            const char *next = strstr(p + 10, "\"tag_name\"");
            const char *segEnd = next ? next : end;
            UpdateInfo u;
            memset(&u, 0, sizeof(u));
            char pre[8] = "", draft[8] = "";
            json_value(p, segEnd, "tag_name", u.tag, sizeof(u.tag));
            json_value(p, segEnd, "prerelease", pre, sizeof(pre));
            json_value(p, segEnd, "draft", draft, sizeof(draft));
            /* the wiidrop.wuhb asset: its digest, size and download link come after its name */
            char name[64];
            const char *a = p;
            while ((a = json_value(a, segEnd, "name", name, sizeof(name))) != NULL) {
                if (strcasecmp(name, "wiidrop.wuhb")) continue;
                const char *lim = a + 4000 < segEnd ? a + 4000 : segEnd;
                char dig[80] = "", sz[24] = "";
                json_value(a, lim, "digest", dig, sizeof(dig));
                json_value(a, lim, "size", sz, sizeof(sz));
                json_value(a, lim, "browser_download_url", u.url, sizeof(u.url));
                if (!strncmp(dig, "sha256:", 7) && strlen(dig + 7) == 64) snprintf(u.sha256, sizeof(u.sha256), "%s", dig + 7);
                u.size = strtoull(sz, NULL, 10);
                break;
            }
            bool ok = u.url[0] && !strncmp(u.url, "https://", 8) && strcmp(draft, "true") &&
                      (allowPre || strcmp(pre, "true"));
            if (ok && version_cmp(u.tag, best.tag) > 0) best = u;
            p = next;
        }
        free(json);
        if (version_cmp(best.tag, WIIDROP_VERSION) > 0 && best.url[0]) {
            gUpd = best;
            found = 1;
        }
    } else {
        snprintf(gUpdErr, sizeof(gUpdErr), "%s", err[0] ? err : "GitHub didn't answer.");
    }
    SDL_AtomicSet(&gUpdState, found ? 2 : json ? 3 : 4);
    return 0;
}

static void update_check_start(const Config *cfg)
{
    gUpdCfg = *cfg;
    gUpdCfg.user[0] = gUpdCfg.password[0] = '\0'; /* your server's login never goes to GitHub */
    gUpdCfg.ca_file[0] = '\0';                   /* and GitHub is always checked against the built-in list */
    gUpdCfg.insecure = 0;
    SDL_AtomicSet(&gUpdState, 1);
    SDL_Thread *t = SDL_CreateThread(update_worker, "update", NULL);
    if (t) SDL_DetachThread(t);
    else SDL_AtomicSet(&gUpdState, 3);
}

/* where the running WiiDrop lives (the usual places), "" if none of them */
static void find_own_wuhb(char *out, size_t n)
{
    static const char *spots[] = {SD_ROOT "/wiiu/apps/wiidrop.wuhb", SD_ROOT "/wiiu/apps/WiiDrop.wuhb",
                                  SD_ROOT "/wiiu/apps/wiidrop/wiidrop.wuhb"};
    out[0] = '\0';
    for (size_t i = 0; i < sizeof(spots) / sizeof(spots[0]); i++)
        if (file_exists(spots[i])) {
            snprintf(out, n, "%s", spots[i]);
            return;
        }
}

typedef struct {
    FILE *f;
    uint64_t got;
    double last;
    bool failed;
} UpdDl;

static int upd_begin(void *ud, int resumed, uint64_t length)
{
    (void)resumed;
    (void)length;
    UpdDl *u = ud;
    if (u->f) rewind(u->f);
    u->got = 0;
    return 0;
}

static int upd_write(void *ud, const void *data, size_t len)
{
    UpdDl *u = ud;
    if (fwrite(data, 1, len, u->f) != len) {
        u->failed = true;
        return -1;
    }
    u->got += len;
    return 0;
}

static int upd_progress(void *ud, uint64_t done, uint64_t total, uint64_t bps)
{
    UpdDl *u = ud;
    if (now_s() - u->last < PROGRESS_INTERVAL) return 0;
    u->last = now_s();
    pump();
    if (gQuit || (read_input().pressed & VPAD_BUTTON_B)) return 1;
    UiProgress p = {0};
    p.stages[0] = "Download";
    p.stages[1] = "Install";
    p.stageCount = 2;
    p.title = "Updating WiiDrop";
    p.detail = gUpd.tag;
    p.done = done;
    p.total = total ? total : gUpd.size;
    p.bytesPerSec = bps;
    p.hint = "Cancel";
    show_progress(&p);
    return 0;
}

/* Download, check and install the new version. Returns true if WiiDrop should close now. */
static bool do_update(void)
{
    char target[300], tmp[320], old[320], err[256] = "";
    find_own_wuhb(target, sizeof(target));
    if (!target[0]) snprintf(target, sizeof(target), SD_ROOT "/wiiu/apps/wiidrop.wuhb");
    snprintf(tmp, sizeof(tmp), "%s.new", target);
    snprintf(old, sizeof(old), "%s.old", target);

    UpdDl u = {0};
    u.f = fopen(tmp, "wb");
    if (!u.f) {
        message_box(UI_MSG_ERROR, "Update failed", "Couldn't write to the SD card.");
        return false;
    }
    transfer_begin();
    DownloadSink sink = {upd_begin, upd_write, upd_progress, &u};
    Config one = gUpdCfg;
    one.connections = 1;
    int rc = net_download(&one, gUpd.url, 0, &sink, err, sizeof(err));
    bool closed = fclose(u.f) == 0;
    transfer_end();

    const char *problem = NULL;
    char why[300];
    if (rc == 1) problem = "Cancelled.";
    else if (rc != 0) problem = err[0] ? err : "The download failed.";
    else if (!closed || u.failed) problem = "Couldn't write to the SD card.";
    else if (gUpd.size && u.got != gUpd.size) problem = "The download is incomplete.";
    if (!problem) { /* it must be a .wuhb, and match GitHub's SHA-256 */
        FILE *f = fopen(tmp, "rb");
        uint8_t magic[4] = {0};
        mbedtls_sha256_context sh;
        mbedtls_sha256_init(&sh);
        mbedtls_sha256_starts_ret(&sh, 0);
        bool first = true;
        static uint8_t buf[256 * 1024];
        size_t n;
        while (f && (n = fread(buf, 1, sizeof(buf), f)) > 0) {
            if (first && n >= 4) memcpy(magic, buf, 4);
            first = false;
            mbedtls_sha256_update_ret(&sh, buf, n);
        }
        if (f) fclose(f);
        uint8_t dg[32];
        mbedtls_sha256_finish_ret(&sh, dg);
        char hex[65];
        for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", dg[i]);
        if (memcmp(magic, "WUHB", 4)) problem = "That isn't a WiiDrop .wuhb file.";
        else if (gUpd.sha256[0] && strcmp(hex, gUpd.sha256)) problem = "The download doesn't match GitHub's checksum.";
    }
    if (problem) {
        remove(tmp);
        snprintf(why, sizeof(why), "%.250s Your current WiiDrop is unchanged.", problem);
        message_box(rc == 1 ? UI_MSG_INFO : UI_MSG_ERROR, rc == 1 ? "Update cancelled" : "Update failed", why);
        return false;
    }

    /* swap it in: replace the old file (or move it aside first) */
    bool swapped = false;
    if (!file_exists(target) || remove(target) == 0) swapped = rename(tmp, target) == 0;
    if (!swapped) {
        remove(old);
        if (rename(target, old) == 0 && rename(tmp, target) == 0) {
            swapped = true;
            remove(old);
        }
    }
    char msg[600];
    if (!swapped) {
        snprintf(msg, sizeof(msg), "The new version was downloaded to %.200s.new but couldn't replace the running app. On a PC, "
                 "delete the old .wuhb and remove \".new\" from the new one's name.", target + strlen(SD_ROOT));
        message_box(UI_MSG_ERROR, "Almost updated", msg);
        return false;
    }
    snprintf(msg, sizeof(msg), "WiiDrop %.39s is installed. WiiDrop closes now: start it again from the Wii U Menu.",
             gUpd.tag[0] == 'v' ? gUpd.tag + 1 : gUpd.tag);
    message_box(UI_MSG_OK, "Updated!", msg);
    return true;
}

/* Ask once, when the check has found something. Returns true if WiiDrop should close (updated). */
static bool update_offer(void)
{
    if (SDL_AtomicGet(&gUpdState) != 2) return false;
    SDL_AtomicSet(&gUpdState, 3); /* only ask once per start */
    char body[300];
    snprintf(body, sizeof(body), "A new version of WiiDrop is out: %.39s (you have %s).%s Update now?",
             gUpd.tag[0] == 'v' ? gUpd.tag + 1 : gUpd.tag, WIIDROP_VERSION,
             gUpd.sha256[0] ? " It's checked against GitHub's checksum before it's installed." : "");
    while (!gQuit) {
        pump();
        Input in = read_input();
        if (in.pressed & VPAD_BUTTON_A) return do_update();
        if (in.pressed & VPAD_BUTTON_B) return false;
        if (!gVisible) continue;
        draw_list_scene();
        ui_confirm("Update available", body, "Update");
        ui_end_frame();
    }
    return false;
}

/* + menu -> Check for updates. Returns true if WiiDrop should close (updated). */
static bool update_check_now(const Config *cfg)
{
    if (SDL_AtomicGet(&gUpdState) != 1) update_check_start(cfg);
    while (!gQuit && SDL_AtomicGet(&gUpdState) == 1) {
        pump();
        Input in = read_input();
        if (in.pressed & (VPAD_BUTTON_A | VPAD_BUTTON_B)) return false; /* keeps checking; it asks if it finds one */
        if (!gVisible) continue;
        draw_list_scene();
        ui_message(UI_MSG_INFO, "Checking for updates", "Asking GitHub for the newest WiiDrop...", "Hide");
        ui_end_frame();
    }
    int st = SDL_AtomicGet(&gUpdState);
    if (st == 2) return update_offer();
    if (st == 4) {
        message_box(UI_MSG_ERROR, "Couldn't check for updates", gUpdErr);
    } else {
        char msg[120];
        snprintf(msg, sizeof(msg), "You have the newest version (%s).", WIIDROP_VERSION);
        message_box(UI_MSG_OK, "Up to date", msg);
    }
    return false;
}

/* ------------------------------------------------------------------ main */

static bool init_video(void)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) return false;
    gWindow = SDL_CreateWindow("WiiDrop", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, UI_W, UI_H, 0);
    if (!gWindow) return false;
    gRenderer = SDL_CreateRenderer(gWindow, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!gRenderer) return false;
    SDL_RenderSetLogicalSize(gRenderer, UI_W, UI_H);

    /* the console's own UI font, straight from system memory */
    void *font = NULL;
    uint32_t fontSize = 0;
    if (!OSGetSharedData(OS_SHAREDDATATYPE_FONT_STANDARD, 0, &font, &fontSize) || !font) return false;
    return ui_init(gRenderer, font, fontSize) == 0;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!init_video()) {
        SDL_Quit();
        return 1;
    }

    SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE); /* software keyboard OK / Cancel */
    mkdir_p(TMP_DIR);
    config_load(CONFIG_PATH, &gCfg);
    Config cfg = gCfg;
    int musicOn = cfg.music != 0;
    load_state(&musicOn);
    music_init(musicOn != 0);
    chime_init();
    KPADInit();
    WPADEnableURCC(TRUE); /* Pro Controllers */
    update_sd_text();

    IMG_Init(IMG_INIT_PNG);
    bool netOk = net_init() == 0;
    gNetOk = netOk;
    {
        bool wantUsb = gUseUsb;
        char err[300];
        if (wantUsb) set_target(true, true);
        else if (gUsbSeen && usbfs_mount(err, sizeof(err)) == 0) set_target(false, true); /* just to see what's on it */
    }
    if (netOk) covers_start(&cfg, COVER_DIR, USBGX_ROOT, USBGX_COVERS, COVER_DIR "/ids.txt");
    bool firstRun = load_entries() < 0;
    if (!netOk) message_box(UI_MSG_ERROR, "Network error", "Could not start networking. Check your internet settings.");
    else if (firstRun)
        message_box(UI_MSG_INFO, "Welcome to WiiDrop",
                    "Created sd:/wiiu/apps/wiidrop/links.txt. Put the SD card in your PC, add one direct download "
                    "link per line, then choose Reload in the + menu.");
    if (gListNote[0]) message_box(UI_MSG_INFO, "Using a saved list", gListNote);
    if (gStartNote[0]) message_box(UI_MSG_INFO, "Installing to the SD card", gStartNote);
    if (netOk && cfg.update_check) update_check_start(&cfg);

    double exitStart = -1;
    while (!gQuit) {
        pump();
        cover_window_update();
        if (exitStart < 0 && update_offer()) { /* updated: close so the new version can start */
            exitStart = now_s();
            gMusicTarget = 0;
            if (gMusic) Mix_FadeOutMusic(400);
        }
        Input in = read_input();

        if (exitStart < 0) {
            if (in.pressed & VPAD_BUTTON_PLUS) {
                int m = menu_screen();
                if (m == MENU_EXIT) {
                    exitStart = now_s();
                    if (gMusic) Mix_FadeOutMusic(400);
                    gMusicTarget = 0; /* (generated music fades in music_update) */
                } else if (m == MENU_RELOAD) {
                    load_entries();
                    update_sd_text();
                    gSel = 0;
                    if (gListNote[0]) message_box(UI_MSG_INFO, "Using a saved list", gListNote);
                } else if (m == MENU_QUEUE && netOk) {
                    run_queue(&cfg);
                } else if (m == MENU_STORAGE) {
                    storage_screen();
                    reapply_view();
                } else if (m == MENU_UPDATE && netOk && update_check_now(&cfg)) {
                    exitStart = now_s(); /* updated: close so the new version can start */
                    gMusicTarget = 0;
                    if (gMusic) Mix_FadeOutMusic(400);
                }
                continue;
            }
            if (entryCount > 0 && (in.pressed & VPAD_BUTTON_MINUS)) open_search();
            int ce = cur_entry();
            if (ce >= 0 && (in.pressed & VPAD_BUTTON_Y)) {
                queue_toggle(ce);
                rebuild_items();
                if (gShow == SHOW_QUEUED) reapply_view();
            }
            if (ce >= 0 && (in.pressed & VPAD_BUTTON_X)) {
                entries[ce].fav = !entries[ce].fav;
                save_hash_list(FAV_PATH, true);
                fill_item(ce);
                if (gShow == SHOW_FAV) reapply_view();
            }
            if (gQuery[0] && (in.pressed & VPAD_BUTTON_B)) apply_search("");
            if (gViewCount > 0) {
                int page = 5; /* the shelf runs sideways: left/right one game, up/down five */
                if (in.pressed & VPAD_BUTTON_RIGHT) gSel++;
                if (in.pressed & VPAD_BUTTON_LEFT) gSel--;
                if (in.pressed & VPAD_BUTTON_DOWN) gSel += page;
                if (in.pressed & VPAD_BUTTON_UP) gSel -= page;
                if (in.pressed & VPAD_BUTTON_ZR) gSel += 100;
                if (in.pressed & VPAD_BUTTON_ZL) gSel -= 100;
                if (gShow == SHOW_QUEUED) { /* the Queued view is the queue: L / R move the game */
                    int qe = cur_entry();
                    if (qe >= 0 && (in.pressed & (VPAD_BUTTON_L | VPAD_BUTTON_R))) {
                        queue_move(qe, (in.pressed & VPAD_BUTTON_L) ? -1 : 1);
                        rebuild_items();
                        reapply_view();
                    }
                } else {
                    if (in.pressed & VPAD_BUTTON_R) gSel = letter_jump(gSel, 1);
                    if (in.pressed & VPAD_BUTTON_L) gSel = letter_jump(gSel, -1);
                }
                if (gSel < 0) gSel = 0;
                if (gSel >= gViewCount) gSel = gViewCount - 1;

                bool activate = (in.pressed & VPAD_BUTTON_A) != 0;
                if (in.tap) {
                    int hit = ui_hit_item(in.tapX, in.tapY, gViewCount);
                    if (hit >= 0 && hit == gSel) activate = true;
                    else if (hit >= 0) gSel = hit;
                }
                if (activate) {
                    details_screen(&cfg, cur_entry());
                    if (gShow != SHOW_ALL || gHideDupes) reapply_view();
                    continue;
                }
            }
        }

        if (gVisible) {
            draw_list_scene();
            if (exitStart >= 0) {
                float k = (float)((now_s() - exitStart) / 0.4);
                ui_fade(k);
                ui_end_frame();
                if (k >= 1) break;
            } else {
                ui_end_frame();
            }
        } else if (exitStart >= 0) {
            break;
        }
    }

    /* Clean shutdown. SDL_Quit() last: SDL's Wii U backend then calls SYSLaunchMenu() and keeps
       processing ProcUI messages until the system says we're done, which is what makes the
       console actually go back to the Wii U Menu. */
    if (netOk) covers_stop();
    usbfs_unmount();
    free_covers();
    music_shutdown();
    ui_shutdown();
    IMG_Quit();
    TTF_Quit();
    SDL_DestroyRenderer(gRenderer);
    SDL_DestroyWindow(gWindow);
    if (netOk) net_shutdown();
    free(items);
    free(entries);
    SDL_Quit();
    return 0;
}
