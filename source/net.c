#include "net.h"

#include <curl/curl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifdef __WIIU__
#include <coreinit/thread.h>
#include <nn/ac/ac_c.h>
#endif

/* what the last download is doing, for the progress screen ("4 connections", or why only one) */
static char gStatus[96];

const char *net_status(void)
{
    return gStatus;
}

#define USER_AGENT "WiiDrop/1.0 (Wii U)"

int net_init(void)
{
#ifdef __WIIU__
    ACInitialize();
    ACConnect();
#endif
    return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK ? 0 : -1;
}

/* After a dropped connection: make sure the console is back on the network before retrying. */
void net_reconnect(void)
{
#ifdef __WIIU__
    ACConnect();
#endif
}

void net_shutdown(void)
{
    curl_global_cleanup();
#ifdef __WIIU__
    ACFinalize();
#endif
}

#ifdef __WIIU__
#include <sys/socket.h>

/* The Wii U's network stack starts every socket with a tiny receive buffer and no TCP window
   scaling, which caps downloads at roughly window / round-trip time (often ~100 KB/s from a server
   that isn't next door). Turn on window scaling + selective ACKs, skip slow start, and ask for the
   biggest receive buffer the system accepts. Called by curl before it connects. */
static int tune_socket(void *ud, curl_socket_t fd, curlsocktype purpose)
{
    (void)ud;
    if (purpose != CURLSOCKTYPE_IPCXN) return CURL_SOCKOPT_OK;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_WINSCALE, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_TCPSACK, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_NOSLOWSTART, &one, sizeof(one));
    static const int sizes[] = {2 * 1024 * 1024, 1024 * 1024, 512 * 1024, 256 * 1024, 128 * 1024};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        int sz = sizes[i];
        if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz)) == 0) break;
    }
    return CURL_SOCKOPT_OK;
}
#endif

static void apply_common(CURL *c, const Config *cfg, const char *url, char *errbuf)
{
#ifdef __WIIU__
    curl_easy_setopt(c, CURLOPT_SOCKOPTFUNCTION, tune_socket);
#endif
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    /* a stalled connection (under 512 bytes/s for 30 s) counts as dropped, so WiiDrop can retry */
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 512L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE, 1L); /* notice silently dead connections sooner */
    curl_easy_setopt(c, CURLOPT_TCP_KEEPIDLE, 20L);
    curl_easy_setopt(c, CURLOPT_TCP_KEEPINTVL, 10L);

    if (cfg->user[0]) {
        curl_easy_setopt(c, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
        curl_easy_setopt(c, CURLOPT_USERNAME, cfg->user);
        curl_easy_setopt(c, CURLOPT_PASSWORD, cfg->password);
    }
    if (cfg->ca_file[0]) curl_easy_setopt(c, CURLOPT_CAINFO, cfg->ca_file);
    if (cfg->insecure) {
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    }
}

/* ---- text fetch ---- */

typedef struct {
    char *data;
    size_t len, cap;
} MemBuf;

static size_t mem_write(char *ptr, size_t size, size_t nmemb, void *ud)
{
    MemBuf *b = ud;
    size_t n = size * nmemb;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 16384;
        while (cap < b->len + n + 1) cap *= 2;
        if (cap > 8 * 1024 * 1024) return 0; /* index this big is surely wrong */
        char *p = realloc(b->data, cap);
        if (!p) return 0;
        b->data = p;
        b->cap = cap;
    }
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = '\0';
    return n;
}

char *net_fetch_text(const Config *cfg, const char *url, char *err, size_t errlen)
{
    char errbuf[CURL_ERROR_SIZE] = {0};
    MemBuf b = {0};
    CURL *c = curl_easy_init();
    if (!c) {
        snprintf(err, errlen, "curl_easy_init failed");
        return NULL;
    }
    apply_common(c, cfg, url, errbuf);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, mem_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    CURLcode rc = curl_easy_perform(c);
    curl_easy_cleanup(c);

    if (rc != CURLE_OK) {
        snprintf(err, errlen, "%s", errbuf[0] ? errbuf : curl_easy_strerror(rc));
        free(b.data);
        return NULL;
    }
    if (!b.data) b.data = calloc(1, 1);
    return b.data;
}

/* ---- small binary fetch ---- */

typedef struct {
    uint8_t *data;
    size_t len, max;
    int full;
    volatile int *cancel;
} CapBuf;

static size_t cap_write(char *ptr, size_t size, size_t nmemb, void *ud)
{
    CapBuf *b = ud;
    size_t n = size * nmemb, room = b->max - b->len;
    size_t take = n < room ? n : room;
    memcpy(b->data + b->len, ptr, take);
    b->len += take;
    if (b->len == b->max) {
        b->full = 1;
        return take == n ? n : 0; /* got everything we wanted: stop here */
    }
    return n;
}

static int cap_progress(void *ud, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d)
{
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    CapBuf *buf = ud;
    return buf->cancel && *buf->cancel;
}

uint8_t *net_fetch_mem(const Config *cfg, const char *url, size_t max, int rangeOnly, volatile int *cancel,
                       size_t *outLen, char *err, size_t errlen)
{
    char errbuf[CURL_ERROR_SIZE] = {0};
    CapBuf b = {0};
    b.max = max;
    b.cancel = cancel;
    b.data = malloc(max);
    *outLen = 0;
    CURL *c = curl_easy_init();
    if (!b.data || !c) {
        snprintf(err, errlen, "out of memory");
        free(b.data);
        if (c) curl_easy_cleanup(c);
        return NULL;
    }
    apply_common(c, cfg, url, errbuf);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 25L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, cap_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, cap_progress);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &b);
    char range[48];
    if (rangeOnly) {
        snprintf(range, sizeof(range), "0-%u", (unsigned)(max - 1));
        curl_easy_setopt(c, CURLOPT_RANGE, range);
    }
    CURLcode rc = curl_easy_perform(c);
    curl_easy_cleanup(c);

    if (rc == CURLE_OK || (rc == CURLE_WRITE_ERROR && b.full)) {
        *outLen = b.len;
        return b.data;
    }
    snprintf(err, errlen, "%s", errbuf[0] ? errbuf : curl_easy_strerror(rc));
    free(b.data);
    return NULL;
}

/* ---- streaming download ---- */

typedef struct {
    CURL *curl;
    DownloadSink *sink;
    uint64_t resume_from;
    int started;
    int resumed;
    int sink_failed;
    int cancelled;
} DlCtx;

static size_t dl_write(char *ptr, size_t size, size_t nmemb, void *ud)
{
    DlCtx *d = ud;
    size_t n = size * nmemb;

    if (!d->started) {
        long code = 0;
        curl_easy_getinfo(d->curl, CURLINFO_RESPONSE_CODE, &code);
        d->resumed = (d->resume_from > 0 && code == 206);
        if (!d->resumed) d->resume_from = 0;
        d->started = 1;
        curl_off_t len = -1;
        curl_easy_getinfo(d->curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &len);
        if (d->sink->begin(d->sink->ud, d->resumed, len > 0 ? (uint64_t)len : 0) != 0) {
            d->sink_failed = 1;
            return 0;
        }
    }
    if (d->sink->write(d->sink->ud, ptr, n) != 0) {
        d->sink_failed = 1;
        return 0;
    }
    return n;
}

static int dl_progress(void *ud, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
    (void)ultotal;
    (void)ulnow;
    DlCtx *d = ud;
    curl_off_t speed = 0;
    curl_easy_getinfo(d->curl, CURLINFO_SPEED_DOWNLOAD_T, &speed);
    uint64_t base = d->started ? d->resume_from : 0;
    uint64_t total = dltotal > 0 ? (uint64_t)dltotal + base : 0;
    if (d->sink->progress(d->sink->ud, (uint64_t)dlnow + base, total, (uint64_t)speed) != 0) {
        d->cancelled = 1;
        return 1;
    }
    return 0;
}

/* ---- parallel download ----
 *
 * One Wii U TCP connection tops out well below what the network can do (small windows, slow
 * stack), so big files are fetched as 4 MB pieces over several connections at once by a worker
 * thread, and handed to the sink strictly in order on the caller's thread. The sink (unzip, ISO ->
 * WBFS conversion, SD writes) never knows the difference, and resume works exactly as before:
 * everything before the piece being handed over is on the SD card.
 */

#include <SDL.h>

#define PAR_CHUNK     (4u * 1024 * 1024)
#define PAR_MIN_SIZE  (16ull * 1024 * 1024) /* smaller files: one connection is fine */
#define PAR_MAX_CONNS 8
#define PAR_TRIES     4                     /* per piece, before the whole download reports an error */

enum { SL_FREE, SL_LOADING, SL_DONE };

typedef struct Par Par;

typedef struct {
    Par *par;
    int state;
    uint64_t off;
    size_t len, got;
    uint8_t *buf;
    CURL *h;
    int tries;
    Uint64 retryAt;
    char errbuf[CURL_ERROR_SIZE];
    long code;
} Slot;

struct Par {
    const Config *cfg;
    const char *url;
    uint64_t total;   /* full file size */
    uint64_t nextOff; /* next piece to start (fetch thread) */
    uint64_t deliver; /* next byte the caller needs */
    int conns, nslots;
    Slot slots[PAR_MAX_CONNS + 3];
    SDL_mutex *mtx;
    SDL_cond *cond;
    CURLM *multi;
    volatile int stop;
    int fatal;        /* a piece failed PAR_TRIES times (or the server misbehaved) */
    char err[256];
    uint64_t rxLocal, rxShared; /* bytes received (for the speed display) */
};

static size_t par_write(char *ptr, size_t size, size_t nmemb, void *ud)
{
    Slot *s = ud;
    size_t n = size * nmemb;
    if (s->got + n > s->len) return 0; /* more than we asked for: the server ignored the range */
    memcpy(s->buf + s->got, ptr, n);
    s->got += n;
    s->par->rxLocal += n;
    return n;
}

static void par_start(Par *p, Slot *s)
{
    if (!s->h) s->h = curl_easy_init();
    if (!s->h) return;
    curl_easy_reset(s->h);
    s->errbuf[0] = '\0';
    apply_common(s->h, p->cfg, p->url, s->errbuf);
    curl_easy_setopt(s->h, CURLOPT_WRITEFUNCTION, par_write);
    curl_easy_setopt(s->h, CURLOPT_WRITEDATA, s);
    curl_easy_setopt(s->h, CURLOPT_BUFFERSIZE, 256L * 1024L);
    char range[64];
    snprintf(range, sizeof(range), "%" PRIu64 "-%" PRIu64, s->off + s->got, s->off + s->len - 1);
    curl_easy_setopt(s->h, CURLOPT_RANGE, range);
    curl_multi_add_handle(p->multi, s->h);
    s->state = SL_LOADING;
}

static int par_thread(void *ud)
{
    Par *p = ud;
#ifdef __WIIU__
    /* its own CPU core: the main thread (core 1) is busy unzipping, converting and writing the SD card */
    OSSetThreadAffinity(OSGetCurrentThread(), OS_THREAD_ATTRIB_AFFINITY_CPU2);
#endif
    while (!p->stop) {
        /* start pieces: at most `conns` loading, inside the window of slots */
        int loading = 0;
        Uint64 now = SDL_GetTicks64();
        SDL_LockMutex(p->mtx);
        for (int i = 0; i < p->nslots; i++)
            if (p->slots[i].state == SL_LOADING && p->slots[i].h) loading++;
        for (int i = 0; i < p->nslots && loading < p->conns; i++) {
            Slot *s = &p->slots[i];
            if (s->state == SL_LOADING && !s->h) { /* waiting to retry */
                if (now >= s->retryAt) {
                    SDL_UnlockMutex(p->mtx);
                    par_start(p, s);
                    SDL_LockMutex(p->mtx);
                    loading++;
                }
                continue;
            }
            if (s->state != SL_FREE || p->nextOff >= p->total) continue;
            s->off = p->nextOff;
            s->len = p->total - p->nextOff < PAR_CHUNK ? (size_t)(p->total - p->nextOff) : PAR_CHUNK;
            s->got = 0;
            s->tries = 0;
            p->nextOff += s->len;
            SDL_UnlockMutex(p->mtx);
            par_start(p, s);
            SDL_LockMutex(p->mtx);
            loading++;
        }
        p->rxShared = p->rxLocal;
        SDL_UnlockMutex(p->mtx);

        int running = 0;
        curl_multi_perform(p->multi, &running);
        CURLMsg *m;
        int left;
        while ((m = curl_multi_info_read(p->multi, &left)) != NULL) {
            if (m->msg != CURLMSG_DONE) continue;
            Slot *s = NULL;
            for (int i = 0; i < p->nslots; i++)
                if (p->slots[i].h == m->easy_handle) s = &p->slots[i];
            if (!s) continue;
            CURLcode rc = m->data.result;
            long code = 0;
            curl_easy_getinfo(s->h, CURLINFO_RESPONSE_CODE, &code);
            curl_multi_remove_handle(p->multi, s->h);
            SDL_LockMutex(p->mtx);
            if (rc == CURLE_OK && code == 206 && s->got == s->len) {
                s->state = SL_DONE;
                SDL_CondBroadcast(p->cond);
            } else if (code == 200 || (rc == CURLE_OK && s->got != s->len) || (code >= 400 && code < 500 && code != 408 && code != 429)) {
                /* server stopped honouring ranges, or refused: no point retrying this piece */
                p->fatal = 1;
                snprintf(p->err, sizeof(p->err), "%s", s->errbuf[0] ? s->errbuf : code == 200 ? "The server stopped "
                         "accepting partial downloads" : "Unexpected reply from the server");
                SDL_CondBroadcast(p->cond);
            } else if (++s->tries >= PAR_TRIES) {
                p->fatal = 1;
                snprintf(p->err, sizeof(p->err), "%s", s->errbuf[0] ? s->errbuf : curl_easy_strerror(rc));
                SDL_CondBroadcast(p->cond);
            } else { /* network hiccup: carry on from what this piece already has, after a moment */
                curl_easy_cleanup(s->h);
                s->h = NULL;
                s->retryAt = SDL_GetTicks64() + 1000u * (Uint64)s->tries;
            }
            SDL_UnlockMutex(p->mtx);
        }
        curl_multi_poll(p->multi, NULL, 0, 50, NULL);
    }
    for (int i = 0; i < p->nslots; i++) {
        Slot *s = &p->slots[i];
        if (s->h) {
            curl_multi_remove_handle(p->multi, s->h);
            curl_easy_cleanup(s->h);
            s->h = NULL;
        }
    }
    return 0;
}

/* What the server says about the file: size and whether it accepts ranges.
   1 = ranges ok (*total set), 0 = no ranges / size unknown, 2 = already complete (416), -1 = error. */
typedef struct {
    uint64_t total;
} Probe;

static size_t probe_header(char *buf, size_t size, size_t nitems, void *ud)
{
    Probe *pr = ud;
    size_t n = size * nitems;
    if (n > 15 && !strncasecmp(buf, "Content-Range:", 14)) {
        char tmp[128];
        size_t k = n < sizeof(tmp) - 1 ? n : sizeof(tmp) - 1;
        memcpy(tmp, buf, k);
        tmp[k] = '\0';
        char *slash = strchr(tmp, '/');
        if (slash && slash[1] != '*') pr->total = strtoull(slash + 1, NULL, 10);
    }
    return n;
}

static size_t probe_body(char *ptr, size_t size, size_t nmemb, void *ud)
{
    (void)ptr;
    (void)ud;
    return size * nmemb; /* 1 byte (or the whole file if ranges are ignored: we abort below) */
}

static int probe_progress(void *ud, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
    (void)ud;
    (void)dltotal;
    (void)ultotal;
    (void)ulnow;
    return dlnow > 4096; /* the server is sending the whole file: stop, we'll do it in one stream */
}

static int probe(const Config *cfg, const char *url, uint64_t from, uint64_t *total, char *err, size_t errlen)
{
    char errbuf[CURL_ERROR_SIZE] = {0};
    Probe pr = {0};
    CURL *c = curl_easy_init();
    if (!c) return -1;
    apply_common(c, cfg, url, errbuf);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 0L);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, probe_header);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &pr);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, probe_body);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, probe_progress);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    char range[48];
    snprintf(range, sizeof(range), "%" PRIu64 "-%" PRIu64, from, from);
    curl_easy_setopt(c, CURLOPT_RANGE, range);
    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    if (code == 416) return 2;
    if (code == 206 && pr.total > 0) {
        *total = pr.total;
        return 1;
    }
    if (code == 200 || code == 206) return 0;
    if (rc == CURLE_OK || rc == CURLE_ABORTED_BY_CALLBACK) {
        if (code >= 400) {
            snprintf(err, errlen, "The requested URL returned error: %ld", code);
            return -1;
        }
        return 0;
    }
    snprintf(err, errlen, "%s", errbuf[0] ? errbuf : curl_easy_strerror(rc));
    return -1;
}

/* 0 ok, 1 cancelled, -1 error, 2 = use the single-connection path instead */
static int download_parallel(const Config *cfg, const char *url, uint64_t from, DownloadSink *sink, char *err,
                             size_t errlen)
{
    uint64_t total = 0;
    int pr = probe(cfg, url, from, &total, err, errlen);
    if (pr == 2) {
        snprintf(gStatus, sizeof(gStatus), "1 connection");
        if (from > 0) return 0; /* nothing left to fetch */
        return 2;
    }
    if (pr < 0) return -1;
    if (pr == 0) {
        snprintf(gStatus, sizeof(gStatus), "1 connection (the server doesn't allow partial downloads)");
        return 2;
    }
    if (total <= from || total - from < PAR_MIN_SIZE) {
        snprintf(gStatus, sizeof(gStatus), "1 connection (small file)");
        return 2;
    }

    static Par p; /* big: keep it off the stack */
    memset(&p, 0, sizeof(p));
    p.cfg = cfg;
    p.url = url;
    p.total = total;
    p.nextOff = p.deliver = from;
    p.conns = cfg->connections < 1 ? 1 : cfg->connections > PAR_MAX_CONNS ? PAR_MAX_CONNS : cfg->connections;
    p.nslots = p.conns + 3; /* a few finished pieces may wait while the SD card catches up */
    for (int i = 0; i < p.nslots; i++) {
        p.slots[i].par = &p;
        p.slots[i].buf = malloc(PAR_CHUNK);
        if (!p.slots[i].buf) {
            p.nslots = i;
            break;
        }
    }
    if (p.nslots < 2) {
        for (int i = 0; i < p.nslots; i++) free(p.slots[i].buf);
        return 2;
    }
    if (p.nslots - 2 < p.conns) p.conns = p.nslots - 2 > 0 ? p.nslots - 2 : 1;
    p.mtx = SDL_CreateMutex();
    p.cond = SDL_CreateCond();
    p.multi = curl_multi_init();
    curl_multi_setopt(p.multi, CURLMOPT_MAX_HOST_CONNECTIONS, (long)p.conns);
    snprintf(gStatus, sizeof(gStatus), "%d connections", p.conns);

    int result = 0;
    if (sink->begin(sink->ud, from > 0, total - from) != 0) {
        if (!err[0]) snprintf(err, errlen, "Could not open output file");
        result = -1;
    }
    SDL_Thread *th = result == 0 ? SDL_CreateThread(par_thread, "download", &p) : NULL;
    if (result == 0 && !th) {
        snprintf(err, errlen, "Could not start the download thread");
        result = -1;
    }

    /* speed: bytes received over the last couple of seconds */
    Uint64 t0 = SDL_GetTicks64();
    uint64_t rx0 = 0, speed = 0;
    while (result == 0 && p.deliver < total) {
        SDL_LockMutex(p.mtx);
        Slot *ready = NULL;
        for (int i = 0; i < p.nslots; i++)
            if (p.slots[i].state == SL_DONE && p.slots[i].off == p.deliver) ready = &p.slots[i];
        int fatal = p.fatal;
        if (!ready && !fatal) SDL_CondWaitTimeout(p.cond, p.mtx, 50);
        uint64_t rx = p.rxShared;
        SDL_UnlockMutex(p.mtx);

        Uint64 t = SDL_GetTicks64();
        if (t - t0 >= 1000) {
            uint64_t inst = (rx - rx0) * 1000 / (t - t0);
            speed = speed ? (speed * 2 + inst) / 3 : inst;
            rx0 = rx;
            t0 = t;
        }
        if (ready) {
            /* hand the piece over in 1 MB bites so the screen keeps updating */
            for (size_t at = 0; at < ready->len && result == 0;) {
                size_t n = ready->len - at < 1024 * 1024 ? ready->len - at : 1024 * 1024;
                if (sink->write(sink->ud, ready->buf + at, n) != 0) {
                    if (!err[0]) snprintf(err, errlen, "Writing to SD card failed (card full or removed?)");
                    result = -1;
                    break;
                }
                at += n;
                if (sink->progress(sink->ud, p.deliver + at, total, speed) != 0) result = 1;
            }
            SDL_LockMutex(p.mtx);
            p.deliver += ready->len;
            ready->state = SL_FREE;
            SDL_UnlockMutex(p.mtx);
            curl_multi_wakeup(p.multi);
            continue;
        }
        if (fatal) {
            snprintf(err, errlen, "%s", p.err);
            result = -1;
            break;
        }
        if (sink->progress(sink->ud, p.deliver, total, speed) != 0) result = 1;
    }

    p.stop = 1;
    if (th) {
        curl_multi_wakeup(p.multi);
        SDL_WaitThread(th, NULL);
    }
    curl_multi_cleanup(p.multi);
    SDL_DestroyCond(p.cond);
    SDL_DestroyMutex(p.mtx);
    for (int i = 0; i < p.nslots; i++) free(p.slots[i].buf);
    return result;
}

/* Size of the file at url (HEAD request), 0 if unknown. */
uint64_t net_fetch_size(const Config *cfg, const char *url)
{
    char errbuf[CURL_ERROR_SIZE] = {0};
    CURL *c = curl_easy_init();
    if (!c) return 0;
    apply_common(c, cfg, url, errbuf);
    curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 8L);
    curl_off_t len = -1;
    if (curl_easy_perform(c) == CURLE_OK) curl_easy_getinfo(c, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &len);
    curl_easy_cleanup(c);
    return len > 0 ? (uint64_t)len : 0;
}

int net_download(const Config *cfg, const char *url, uint64_t resume_from,
                 DownloadSink *sink, char *err, size_t errlen)
{
    if (cfg->connections > 1) gStatus[0] = '\0'; /* set once the server has answered */
    else snprintf(gStatus, sizeof(gStatus), "1 connection (connections = 1 in config.ini)");
    if (cfg->connections > 1) {
        int r = download_parallel(cfg, url, resume_from, sink, err, errlen);
        if (r != 2) return r;
        err[0] = '\0';
    }
    char errbuf[CURL_ERROR_SIZE] = {0};
    CURL *c = curl_easy_init();
    if (!c) {
        snprintf(err, errlen, "curl_easy_init failed");
        return -1;
    }

    DlCtx d = {.curl = c, .sink = sink, .resume_from = resume_from};

    apply_common(c, cfg, url, errbuf);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, dl_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &d);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, dl_progress);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &d);
    curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 512L * 1024L);
    if (resume_from > 0) curl_easy_setopt(c, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)resume_from);

    CURLcode rc = curl_easy_perform(c);

    /* Server replied 416 (range not satisfiable) means we already have the whole file. */
    long code = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);

    if (rc == CURLE_OK) {
        if (!d.started) {
            /* empty body: still let the sink set itself up */
            if (sink->begin(sink->ud, resume_from > 0, 0) != 0) {
                snprintf(err, errlen, "Could not open output file");
                return -1;
            }
        }
        return 0;
    }
    if (d.cancelled) return 1;
    if (d.sink_failed) {
        /* the sink fills in its own, more specific error */
        if (!err[0]) snprintf(err, errlen, "Writing to SD card failed (card full or removed?)");
        return -1;
    }
    if (code == 416 && resume_from > 0) return 0;
    snprintf(err, errlen, "%s", errbuf[0] ? errbuf : curl_easy_strerror(rc));
    return -1;
}
