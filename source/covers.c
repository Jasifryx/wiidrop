#include "covers.h"

#include <SDL.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "disc.h"
#include "net.h"
#include "rvz.h"
#include "zipstream.h"

#define MAX_COVER_BYTES (2 * 1024 * 1024)

static SDL_Thread *thread;
static SDL_mutex *mtx;
static volatile int stopFlag, pauseFlag;

/* requests (in priority order), the one being worked on, and finished results */
static CoverJob *pending, *results;
static int nPending, capPending, nResults, capResults;
static int inflightIndex = -1;
static char inflightHash[9];

static Config userCfg, plainCfg;
static char cacheDir[256], loaderRoot[256], loaderDir[256], idCachePath[256];

/* ------------------------------------------------------------------ ID cache (hash table) */

typedef struct {
    char hash[9], id[7], title[65];
} IdEntry;
static IdEntry *idCache;
static int idCount, idCap;
static int *idTable; /* open addressing: entry index + 1, 0 = empty */
static int idTableCap;

/* A disc ID is 6 capital letters / digits. IDs from the title list or ids.txt end up in file names
   (covers/<ID>.png), so anything else is ignored. */
static int valid_id(const char *s)
{
    for (int i = 0; i < 6; i++)
        if (!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9'))) return 0;
    return s[6] == '\0' || s[6] == ' ' || s[6] == '\t';
}

static uint32_t key_of(const char *hash)
{
    return (uint32_t)strtoul(hash, NULL, 16) * 2654435761u;
}

static void id_table_insert(int idx)
{
    uint32_t k = key_of(idCache[idx].hash);
    for (int i = (int)(k & (uint32_t)(idTableCap - 1));; i = (i + 1) & (idTableCap - 1)) {
        if (!idTable[i]) {
            idTable[i] = idx + 1;
            return;
        }
    }
}

static void id_table_rebuild(int cap)
{
    free(idTable);
    idTable = calloc((size_t)cap, sizeof(int));
    idTableCap = idTable ? cap : 0;
    for (int i = 0; i < idCount && idTable; i++) id_table_insert(i);
}

static int id_find(const char *hash)
{
    if (!idTableCap) return -1;
    uint32_t k = key_of(hash);
    for (int i = (int)(k & (uint32_t)(idTableCap - 1));; i = (i + 1) & (idTableCap - 1)) {
        if (!idTable[i]) return -1;
        if (!strcmp(idCache[idTable[i] - 1].hash, hash)) return idTable[i] - 1;
    }
}

static void id_cache_add(const char *hash, const char *id, const char *title)
{
    int existing = id_find(hash);
    if (existing >= 0) return;
    if (idCount == idCap) {
        int cap = idCap ? idCap * 2 : 256;
        IdEntry *n = realloc(idCache, (size_t)cap * sizeof(IdEntry));
        if (!n) return;
        idCache = n;
        idCap = cap;
    }
    IdEntry *e = &idCache[idCount];
    snprintf(e->hash, sizeof(e->hash), "%s", hash);
    snprintf(e->id, sizeof(e->id), "%s", id);
    snprintf(e->title, sizeof(e->title), "%s", title);
    idCount++;
    if (idCount * 2 > idTableCap) id_table_rebuild(idTableCap ? idTableCap * 2 : 1024);
    else id_table_insert(idCount - 1);
}

static void id_cache_load(void)
{
    FILE *f = fopen(idCachePath, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *a = strtok(line, "\t"), *b = strtok(NULL, "\t"), *c = strtok(NULL, "\t");
        if (a && b && valid_id(b)) id_cache_add(a, b, c ? c : "");
    }
    fclose(f);
}

int covers_cached_id(const char *hash, char id[7], char title[65])
{
    int found = 0;
    if (mtx) SDL_LockMutex(mtx);
    int i = id_find(hash);
    if (i >= 0) {
        snprintf(id, 7, "%s", idCache[i].id);
        snprintf(title, 65, "%s", idCache[i].title);
        found = 1;
    }
    if (mtx) SDL_UnlockMutex(mtx);
    return found;
}

/* ------------------------------------------------------------------ cover cache contents (hash set) */

static uint64_t *haveSet; /* packed 6-char IDs, 0 = empty */
static int haveCount, haveCap;

static uint64_t pack_id(const char *id)
{
    uint64_t v = 0;
    for (int i = 0; i < 6 && id[i]; i++) v = (v << 8) | (uint8_t)id[i];
    return v;
}

static void have_insert_raw(uint64_t v)
{
    uint32_t h = (uint32_t)(v ^ (v >> 29)) * 2654435761u;
    for (int i = (int)(h & (uint32_t)(haveCap - 1));; i = (i + 1) & (haveCap - 1)) {
        if (haveSet[i] == v) return;
        if (!haveSet[i]) {
            haveSet[i] = v;
            haveCount++;
            return;
        }
    }
}

static void have_add(const char *id)
{
    uint64_t v = pack_id(id);
    if (!v) return;
    if ((haveCount + 1) * 2 > haveCap) {
        int cap = haveCap ? haveCap * 2 : 1024;
        uint64_t *old = haveSet;
        int oldCap = haveCap;
        haveSet = calloc((size_t)cap, sizeof(uint64_t));
        if (!haveSet) {
            haveSet = old;
            return;
        }
        haveCap = cap;
        haveCount = 0;
        for (int i = 0; i < oldCap; i++)
            if (old[i]) have_insert_raw(old[i]);
        free(old);
    }
    have_insert_raw(v);
}

int covers_have(const char *id)
{
    if (!id || !id[0]) return 0;
    int found = 0;
    if (mtx) SDL_LockMutex(mtx);
    if (haveCap) {
        uint64_t v = pack_id(id);
        uint32_t h = (uint32_t)(v ^ (v >> 29)) * 2654435761u;
        for (int i = (int)(h & (uint32_t)(haveCap - 1));; i = (i + 1) & (haveCap - 1)) {
            if (!haveSet[i]) break;
            if (haveSet[i] == v) {
                found = 1;
                break;
            }
        }
    }
    if (mtx) SDL_UnlockMutex(mtx);
    return found;
}

/* one directory listing instead of a file check per game */
static void scan_cover_dir(void)
{
    DIR *d = opendir(cacheDir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *n = de->d_name;
        size_t len = strlen(n);
        if (len == 10 && !strcmp(n + 6, ".png")) {
            char id[7];
            memcpy(id, n, 6);
            id[6] = '\0';
            have_add(id);
        }
    }
    closedir(d);
}

/* ------------------------------------------------------------------ helpers */

static int exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0;
}

void covers_path(const char *id, char *out, unsigned n)
{
    snprintf(out, n, "%s/%s.png", cacheDir, id);
}

static int write_file(const char *path, const void *data, size_t len)
{
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s.part", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    size_t w = fwrite(data, 1, len, f);
    if (fclose(f) != 0 || w != len) {
        remove(tmp);
        return -1;
    }
    remove(path);
    return rename(tmp, path);
}

static void copy_file(const char *from, const char *to)
{
    FILE *f = fopen(from, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n > 0 && n <= MAX_COVER_BYTES) {
        void *buf = malloc((size_t)n);
        if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n) {
            fclose(f);
            f = NULL;
            write_file(to, buf, (size_t)n);
        }
        free(buf);
    }
    if (f) fclose(f);
}

/* GameTDB groups covers by language; pick the likely one from the ID's region letter, then fall back. */
static int regions_for(const char *id, const char *out[6])
{
    const char *first;
    switch (id[3]) {
    case 'E': first = "US"; break;
    case 'J': first = "JA"; break;
    case 'K': first = "KO"; break;
    case 'W': first = "ZHTW"; break;
    case 'D': first = "DE"; break;
    case 'F': first = "FR"; break;
    case 'S': first = "ES"; break;
    case 'I': first = "IT"; break;
    case 'H': first = "NL"; break;
    case 'U': first = "AU"; break;
    default: first = "EN"; break;
    }
    static const char *fallback[] = {"EN", "US", "JA"};
    int n = 0;
    out[n++] = first;
    for (int i = 0; i < 3; i++)
        if (strcmp(fallback[i], first)) out[n++] = fallback[i];
    return n;
}

/* ------------------------------------------------------------------ GameTDB title lookup */

#define TITLES_URL "https://www.gametdb.com/wiitdb.txt?LANG=EN"
#define TITLES_URL_HTTP "http://www.gametdb.com/wiitdb.txt?LANG=EN"

typedef struct {
    char key[64]; /* normalized title */
    char id[7];
    char title[65];
} TitleRow;

static TitleRow *titles;
static int nTitles;
static Uint64 titlesRetryAt; /* failed download: try again after this */

/* "Legend of Zelda, The - Twilight Princess" / "The Legend of Zelda: Twilight Princess"
   -> "legendofzeldatwilightprincess" (lowercase letters and digits; a leading "the" and "& / and"
   differences are ignored). */
static void norm_title(const char *in, char *out, size_t n)
{
    char tmp[256];
    size_t j = 0;
    for (const char *s = in; *s && j + 1 < sizeof(tmp); s++) {
        if (*s == '&') {
            const char *a = "and";
            while (*a && j + 1 < sizeof(tmp)) tmp[j++] = *a++;
            continue;
        }
        unsigned char c = (unsigned char)*s;
        if (c == 0xC3 && s[1]) { /* UTF-8 accented Latin letters: "Pokémon" matches "Pokemon" */
            static const char fold[64] = "aaaaaaaceeeeiiiidnooooo/ouuuuyty"
                                         "aaaaaaaceeeeiiiidnooooo/ouuuuyty";
            unsigned char d = (unsigned char)s[1];
            s++;
            if (d >= 0x80 && d < 0xC0) {
                char f = fold[d - 0x80];
                if (f != '/') tmp[j++] = f;
            }
            continue;
        }
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) tmp[j++] = (char)c;
    }
    tmp[j] = '\0';
    const char *t = tmp;
    if (!strncmp(t, "the", 3) && strlen(t) > 6) t += 3;
    size_t k = strlen(t) < n - 1 ? strlen(t) : n - 1; /* long titles: the first 63 letters decide */
    memcpy(out, t, k);
    out[k] = '\0';
}

static int cmp_row(const void *a, const void *b)
{
    return strcmp(((const TitleRow *)a)->key, ((const TitleRow *)b)->key);
}

int covers_load_titles(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int cap = 0, n = 0;
    TitleRow *rows = NULL;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *eq = strstr(line, " = ");
        if (!eq || eq - line != 6 || !valid_id(line)) continue; /* 6-character disc IDs only */
        if (n == cap) {
            cap = cap ? cap * 2 : 4096;
            TitleRow *r = realloc(rows, (size_t)cap * sizeof(TitleRow));
            if (!r) break;
            rows = r;
        }
        TitleRow *r = &rows[n];
        memcpy(r->id, line, 6);
        r->id[6] = '\0';
        snprintf(r->title, sizeof(r->title), "%s", eq + 3);
        norm_title(eq + 3, r->key, sizeof(r->key));
        if (r->key[0]) n++;
    }
    fclose(f);
    if (n < 100) { /* not the real list (an error page?) */
        free(rows);
        return -1;
    }
    qsort(rows, (size_t)n, sizeof(TitleRow), cmp_row);
    free(titles);
    titles = rows;
    nTitles = n;
    return 0;
}

static int ci_has(const char *s, const char *word)
{
    size_t n = strlen(word);
    for (; *s; s++)
        if (!strncasecmp(s, word, n)) return 1;
    return 0;
}

/* region letters wanted by Redump / No-Intro tags: (USA) -> E, (Europe) / (UK) ... -> P, ... */
static char name_region(const char *base)
{
    if (ci_has(base, "(USA") || ci_has(base, "USA)") || ci_has(base, "USA,") || ci_has(base, "(NTSC-U") ||
        ci_has(base, "(Canada"))
        return 'E';
    if (ci_has(base, "(Europe") || ci_has(base, "Europe)") || ci_has(base, "(PAL") || ci_has(base, "(UK") ||
        ci_has(base, "(Germany") || ci_has(base, "(France") || ci_has(base, "(Spain") || ci_has(base, "(Italy") ||
        ci_has(base, "(Australia") || ci_has(base, "(Netherlands") || ci_has(base, "(Scandinavia"))
        return 'P';
    if (ci_has(base, "(Japan") || ci_has(base, "(NTSC-J")) return 'J';
    if (ci_has(base, "(Korea")) return 'K';
    return 0;
}

/* Split a title into lowercase words for fuzzy matching: "Disney-Pixar Toy Story 3" -> disney pixar toy story 3.
   Little words (the, a, of, and) are dropped and II/III/IV become 2/3/4. Returns the word count. */
#define MAX_WORDS 24
static int title_words(const char *in, char words[MAX_WORDS][24])
{
    char norm[256];
    size_t j = 0;
    for (const char *s = in; *s && j + 2 < sizeof(norm); s++) {
        unsigned char c = (unsigned char)*s;
        if (c == 0xC3 && s[1]) { /* accented letters, as in norm_title */
            static const char fold[64] = "aaaaaaaceeeeiiiidnooooo/ouuuuyty"
                                         "aaaaaaaceeeeiiiidnooooo/ouuuuyty";
            unsigned char dd = (unsigned char)s[1];
            s++;
            if (dd >= 0x80 && dd < 0xC0 && fold[dd - 0x80] != '/') norm[j++] = fold[dd - 0x80];
            continue;
        }
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + 32);
        if (c == '\'') continue; /* Kirby's -> kirbys */
        norm[j++] = ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) ? (char)c : ' ';
    }
    norm[j] = '\0';
    int n = 0;
    for (char *p = norm; *p && n < MAX_WORDS;) { /* split on spaces (no strtok: this runs on the worker thread) */
        while (*p == ' ') p++;
        if (!*p) break;
        char *t = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = '\0';
        if (!strcmp(t, "the") || !strcmp(t, "a") || !strcmp(t, "an") || !strcmp(t, "of") || !strcmp(t, "and")) continue;
        if (!strcmp(t, "ii")) t = "2";
        else if (!strcmp(t, "iii")) t = "3";
        else if (!strcmp(t, "iv")) t = "4";
        snprintf(words[n++], 24, "%.23s", t);
    }
    return n;
}

static int common_words(char a[MAX_WORDS][24], int na, char b[MAX_WORDS][24], int nb)
{
    int c = 0;
    bool used[MAX_WORDS] = {0};
    for (int i = 0; i < na; i++)
        for (int k = 0; k < nb; k++)
            if (!used[k] && !strcmp(a[i], b[k])) {
                used[k] = true;
                c++;
                break;
            }
    return c;
}

/* among rows lo..hi (same title), pick the region: exact letter, any European letter for PAL, then E, P, J */
static TitleRow *pick_region(TitleRow *lo, TitleRow *hi, char want)
{
    TitleRow *best = NULL;
    for (TitleRow *r = lo; r <= hi && !best; r++)
        if (want && r->id[3] == want) best = r;
    if (!best && want == 'P')
        for (TitleRow *r = lo; r <= hi && !best; r++)
            if (strchr("DFSIXYUH", r->id[3])) best = r;
    for (const char *o = "EPJ"; *o && !best; o++)
        for (TitleRow *r = lo; r <= hi && !best; r++)
            if (r->id[3] == *o) best = r;
    return best ? best : lo;
}

static char lastLookup[96]; /* the cleaned-up name of the last failed lookup (for the details screen) */

int covers_id_from_name(const char *fileName, char id[7], char title[65])
{
    if (!nTitles || !fileName || !fileName[0]) return 0;
    char base[256];
    snprintf(base, sizeof(base), "%s", fileName);
    /* drop known extensions: "Game (USA).rvz.zip" -> "Game (USA)" */
    static const char *exts[] = {".zip", ".7z", ".rvz", ".iso", ".wbfs", ".gcm", ".wia", ".nkit", ".ciso", ".gcz"};
    for (int k = 0; k < 3; k++)
        for (size_t x = 0; x < sizeof(exts) / sizeof(exts[0]); x++) {
            size_t bl = strlen(base), el = strlen(exts[x]);
            if (bl > el && !strcasecmp(base + bl - el, exts[x])) base[bl - el] = '\0';
        }
    char want = name_region(base);

    /* remove (...) and [...] tags; dots and underscores between words count as spaces */
    char clean[256];
    size_t j = 0;
    int depth = 0;
    for (const char *s = base; *s && j + 1 < sizeof(clean); s++) {
        if (*s == '(' || *s == '[') depth++;
        else if ((*s == ')' || *s == ']') && depth > 0) depth--;
        else if (!depth) clean[j++] = (*s == '_' || (*s == '.' && s[1] != ' ' && s[1] != '\0')) ? ' ' : *s;
    }
    clean[j] = '\0';
    /* "Legend of Zelda, The - Twilight Princess" -> "The Legend of Zelda - Twilight Princess" */
    char moved[300];
    char *the = strstr(clean, ", The");
    if (the && (the[5] == '\0' || the[5] == ' ' || the[5] == ':')) {
        snprintf(moved, sizeof(moved), "The %.*s%s", (int)(the - clean), clean, the + 5);
    } else {
        snprintf(moved, sizeof(moved), "%s", clean);
    }
    snprintf(lastLookup, sizeof(lastLookup), "%.95s", moved);
    TitleRow key;
    norm_title(moved, key.key, sizeof(key.key));
    if (strlen(key.key) < 3) return 0;

    /* 1. exact (ignoring spaces, punctuation and case) */
    TitleRow *hit = bsearch(&key, titles, (size_t)nTitles, sizeof(TitleRow), cmp_row);
    TitleRow *best = NULL;
    if (hit) {
        TitleRow *lo = hit, *hi = hit;
        while (lo > titles && !strcmp(lo[-1].key, key.key)) lo--;
        while (hi < titles + nTitles - 1 && !strcmp(hi[1].key, key.key)) hi++;
        best = pick_region(lo, hi, want);
    } else {
        /* 2. close enough: most words shared (Dice score >= 0.75, at least 2 words in common), e.g.
              "Disney-Pixar Toy Story 3" -> "Toy Story 3". "Mario Party 8" never becomes "Mario Party 9". */
        char qw[MAX_WORDS][24], tw[MAX_WORDS][24];
        int nq = title_words(moved, qw);
        if (nq == 0) return 0;
        double bestScore = 0;
        const char *bestKey = NULL;
        bool ambiguous = false;
        for (int i = 0; i < nTitles; i++) {
            if (i > 0 && !strcmp(titles[i].key, titles[i - 1].key)) continue; /* same title, other region */
            int nt = title_words(titles[i].title, tw);
            if (nt == 0) continue;
            int c = common_words(qw, nq, tw, nt);
            if (c < 2) continue;
            double score = 2.0 * c / (nq + nt);
            if (score > bestScore + 1e-9) {
                bestScore = score;
                bestKey = titles[i].key;
                ambiguous = false;
            } else if (score > bestScore - 1e-9 && bestKey && strcmp(bestKey, titles[i].key)) {
                ambiguous = true; /* two different games fit equally well: don't guess */
            }
        }
        if (!bestKey || bestScore < 0.75 || ambiguous) return 0;
        snprintf(key.key, sizeof(key.key), "%s", bestKey);
        hit = bsearch(&key, titles, (size_t)nTitles, sizeof(TitleRow), cmp_row);
        if (!hit) return 0;
        TitleRow *lo = hit, *hi = hit;
        while (lo > titles && !strcmp(lo[-1].key, key.key)) lo--;
        while (hi < titles + nTitles - 1 && !strcmp(hi[1].key, key.key)) hi++;
        best = pick_region(lo, hi, want);
    }
    snprintf(id, 7, "%s", best->id);
    snprintf(title, 65, "%s", best->title);
    return 1;
}

/* The title list: from the SD card, else downloaded from GameTDB (https, then plain http). Worker thread only.
   A failed download is tried again a minute later, and jobs that needed it are asked again. */
static char titlesErr[96];

static int ensure_titles(void)
{
    if (nTitles) return 1;
    if (SDL_GetTicks64() < titlesRetryAt) return 0;
    char path[300];
    snprintf(path, sizeof(path), "%s/titles.txt", cacheDir);
    if (covers_load_titles(path) == 0) return 1;
    static const char *urls[] = {TITLES_URL, TITLES_URL_HTTP};
    char err[256] = "";
    for (int i = 0; i < 2 && !stopFlag; i++) {
        char *txt = net_fetch_text(&plainCfg, urls[i], err, sizeof(err));
        if (txt) {
            int wr = write_file(path, txt, strlen(txt));
            free(txt);
            if (covers_load_titles(path) == 0) return 1;
            snprintf(err, sizeof(err), wr ? "couldn't save it to the SD card" : "it wasn't the title list");
        }
    }
    snprintf(titlesErr, sizeof(titlesErr), "%s", err);
    titlesRetryAt = SDL_GetTicks64() + 60 * 1000;
    return 0;
}

/* ------------------------------------------------------------------ worker */

static void remember_id(CoverJob *job, const char *id, const char *title)
{
    snprintf(job->id, sizeof(job->id), "%s", id);
    snprintf(job->title, sizeof(job->title), "%s", title);
    SDL_LockMutex(mtx);
    id_cache_add(job->hash, id, title);
    SDL_UnlockMutex(mtx);
    FILE *f = fopen(idCachePath, "a");
    if (f) {
        fprintf(f, "%s\t%s\t%s\n", job->hash, id, title);
        fclose(f);
    }
}

static void process(CoverJob *job)
{
    char err[256];
    size_t len;

    /* 1. find the game ID from the first bytes of the file on the user's server */
    if (!job->id[0] && job->peek) {
        /* a zip: its first 64 KB normally hold the start of the game file, compressed */
        size_t want = job->peek == PEEK_ZIP ? 65536 : DISC_HEADER_NEED;
        err[0] = '\0';
        uint8_t *buf = net_fetch_mem(&userCfg, job->url, want, 1, &stopFlag, &len, err, sizeof(err));
        uint8_t *hdr = buf;
        size_t hlen = len;
        static uint8_t inner[0x1000];
        if (buf && job->peek == PEEK_ZIP) {
            hdr = zs_peek_game(buf, len, inner, sizeof(inner), &hlen) == 0 ? inner : NULL;
            if (!hdr) snprintf(job->why, sizeof(job->why), "Couldn't read the game inside the zip.");
        }
        if (!buf) {
            snprintf(job->why, sizeof(job->why), "Couldn't reach the file: %.80s", err);
            if (!strstr(err, "returned error: 4")) job->retry = 1;
        }
        if (hdr && hlen >= 0x80) {
            DiscInfo info;
            const uint8_t *rvzHdr = rvz_peek_disc_header(hdr, hlen); /* RVZ keeps the disc header up front */
            DiscKind k = rvzHdr ? disc_identify(rvzHdr, 0x80, &info) : disc_identify(hdr, hlen, &info);
            if (k != DISC_UNKNOWN) remember_id(job, info.id, info.title);
            else if (!job->why[0]) snprintf(job->why, sizeof(job->why), "The file doesn't start like a game.");
        }
        free(buf);
    }
    /* still unknown (a .7z, or an odd zip): match the file name against GameTDB's title list */
    if (!job->id[0] && !stopFlag) {
        if (!ensure_titles()) {
            snprintf(job->why, sizeof(job->why), "Couldn't download GameTDB's title list (%.60s). Retrying.", titlesErr);
            job->retry = 1;
        } else {
            char id[7], title[65];
            if (covers_id_from_name(job->name, id, title) || covers_id_from_name(job->altName, id, title)) {
                remember_id(job, id, title);
                job->retry = 0;
            } else {
                snprintf(job->why, sizeof(job->why), "No GameTDB title matches \"%.80s\".", lastLookup);
            }
        }
    }
    if (!job->id[0] || stopFlag) return;
    job->why[0] = '\0';
    job->retry = 0;

    /* 2. cover art from GameTDB (no credentials: those are only for the user's own host) */
    char path[300];
    covers_path(job->id, path, sizeof(path));
    if (covers_have(job->id)) {
        job->hasCover = 1;
    } else {
        const char *regions[6];
        int nr = regions_for(job->id, regions);
        for (int i = 0; i < nr && !job->hasCover && !stopFlag; i++) {
            char url[200];
            snprintf(url, sizeof(url), "https://art.gametdb.com/wii/cover/%s/%s.png", regions[i], job->id);
            uint8_t *png = net_fetch_mem(&plainCfg, url, MAX_COVER_BYTES, 0, &stopFlag, &len, err, sizeof(err));
            if (png && len > 8 && !memcmp(png, "\x89PNG\r\n\x1a\n", 8) && write_file(path, png, len) == 0) {
                job->hasCover = 1;
                SDL_LockMutex(mtx);
                have_add(job->id);
                SDL_UnlockMutex(mtx);
            }
            free(png);
        }
        if (!job->hasCover) snprintf(job->why, sizeof(job->why), "GameTDB has no cover for %s.", job->id);
    }

    /* 3. optional copy for the loader, so it doesn't have to download it again */
    if (job->hasCover && loaderDir[0] && exists(loaderRoot)) {
        char dst[300];
        snprintf(dst, sizeof(dst), "%s/%s.png", loaderDir, job->id);
        if (!exists(dst)) {
            char parent[256];
            snprintf(parent, sizeof(parent), "%s", loaderDir);
            char *slash = strrchr(parent, '/');
            if (slash) {
                *slash = '\0';
                mkdir(parent, 0777);
            }
            mkdir(loaderDir, 0777);
            copy_file(path, dst);
        }
    }
}

static int push_result(const CoverJob *job)
{
    if (nResults == capResults) {
        int cap = capResults ? capResults * 2 : 32;
        CoverJob *n = realloc(results, (size_t)cap * sizeof(CoverJob));
        if (!n) return -1;
        results = n;
        capResults = cap;
    }
    results[nResults++] = *job;
    return 0;
}

static int worker(void *arg)
{
    (void)arg;
    CoverJob job;
    while (!stopFlag) {
        if (pauseFlag) {
            SDL_Delay(200);
            continue;
        }
        int have = 0;
        SDL_LockMutex(mtx);
        if (nPending > 0) {
            job = pending[0];
            memmove(pending, pending + 1, (size_t)(nPending - 1) * sizeof(CoverJob));
            nPending--;
            inflightIndex = job.index;
            snprintf(inflightHash, sizeof(inflightHash), "%s", job.hash);
            have = 1;
        }
        SDL_UnlockMutex(mtx);

        if (!have) {
            SDL_Delay(100);
            continue;
        }
        process(&job);

        SDL_LockMutex(mtx);
        push_result(&job);
        inflightIndex = -1;
        inflightHash[0] = '\0';
        SDL_UnlockMutex(mtx);
    }
    return 0;
}

/* ------------------------------------------------------------------ API */

void covers_start(const Config *cfg, const char *cache, const char *lRoot, const char *lDir, const char *idPath)
{
    userCfg = *cfg;
    memset(&plainCfg, 0, sizeof(plainCfg));
    plainCfg.insecure = cfg->insecure;
    snprintf(plainCfg.ca_file, sizeof(plainCfg.ca_file), "%s", cfg->ca_file);
    snprintf(cacheDir, sizeof(cacheDir), "%s", cache);
    snprintf(loaderRoot, sizeof(loaderRoot), "%s", lRoot ? lRoot : "");
    snprintf(loaderDir, sizeof(loaderDir), "%s", lDir ? lDir : "");
    snprintf(idCachePath, sizeof(idCachePath), "%s", idPath);
    mkdir(cacheDir, 0777);
    id_cache_load();
    scan_cover_dir();
    mtx = SDL_CreateMutex();
    stopFlag = 0;
    thread = SDL_CreateThread(worker, "covers", NULL);
}

void covers_stop(void)
{
    stopFlag = 1;
    if (thread) SDL_WaitThread(thread, NULL);
    thread = NULL;
    if (mtx) SDL_DestroyMutex(mtx);
    mtx = NULL;
    free(pending);
    free(results);
    free(idCache);
    free(idTable);
    free(haveSet);
    free(titles);
    titles = NULL;
    nTitles = 0;
    pending = results = NULL;
    idCache = NULL;
    idTable = NULL;
    haveSet = NULL;
    nPending = capPending = nResults = capResults = 0;
    idCount = idCap = idTableCap = haveCount = haveCap = 0;
}

void covers_request(const CoverJob *list, int count)
{
    if (!mtx) return;
    SDL_LockMutex(mtx);
    if (count > capPending) {
        CoverJob *n = realloc(pending, (size_t)count * sizeof(CoverJob));
        if (n) {
            pending = n;
            capPending = count;
        }
    }
    nPending = 0;
    for (int i = 0; i < count && i < capPending; i++) {
        if (list[i].index == inflightIndex && !strcmp(list[i].hash, inflightHash)) continue; /* already running */
        pending[nPending++] = list[i];
    }
    SDL_UnlockMutex(mtx);
}

int covers_poll(CoverJob *out, int n)
{
    if (!mtx) return 0;
    int got = 0;
    SDL_LockMutex(mtx);
    if (nResults > 0) {
        got = nResults < n ? nResults : n;
        memcpy(out, results, (size_t)got * sizeof(CoverJob));
        memmove(results, results + got, (size_t)(nResults - got) * sizeof(CoverJob));
        nResults -= got;
    }
    SDL_UnlockMutex(mtx);
    return got;
}

void covers_pause(int paused)
{
    pauseFlag = paused;
}
