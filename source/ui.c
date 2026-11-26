#include "ui.h"

#include <SDL_ttf.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------------ palette */

typedef struct { Uint8 r, g, b, a; } Col;
#define RGBA(r, g, b, a) ((Col){(r), (g), (b), (a)})

/* Tidepool: deep teal water, aqua light; warm colours only where something needs attention */
static const Col C_TEXT    = {232, 250, 245, 255};
static const Col C_TEXT2   = {162, 214, 202, 255};
static const Col C_MUTED   = {98, 146, 140, 255};
static const Col C_ACCENT  = {93, 202, 165, 255};  /* aqua */
static const Col C_ACCENT2 = {159, 225, 203, 255}; /* foam */
static const Col C_GREEN   = {151, 214, 120, 255};
static const Col C_AMBER   = {250, 199, 117, 255};
static const Col C_RED     = {240, 153, 123, 255}; /* coral */
static const Col C_BLUE    = {133, 183, 235, 255};

static Col alpha(Col c, int a)
{
    c.a = (Uint8)(a < 0 ? 0 : a > 255 ? 255 : a);
    return c;
}

static Col mix(Col a, Col b, float t)
{
    Col c;
    c.r = (Uint8)(a.r + (b.r - a.r) * t);
    c.g = (Uint8)(a.g + (b.g - a.g) * t);
    c.b = (Uint8)(a.b + (b.b - a.b) * t);
    c.a = (Uint8)(a.a + (b.a - a.a) * t);
    return c;
}

/* ------------------------------------------------------------------ state */

static SDL_Renderer *R;
enum { F_XS, F_S, F_M, F_L, F_LB, F_XL, F_COUNT };
static TTF_Font *fonts[F_COUNT];
static SDL_Texture *texCircle; /* anti-aliased white disc */
static SDL_Texture *texSoft;   /* soft radial blob */
static SDL_Texture *texShadow; /* 9-slice soft rectangle */

static double T, DT, lastT = -1;
static unsigned msgHash;
static double msgStart;

/* ------------------------------------------------------------------ generated textures */

static SDL_Texture *make_texture(int w, int h, float (*fn)(float x, float y, int w, int h))
{
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
    if (!s) return NULL;
    Uint8 *px = s->pixels;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float a = fn((float)x, (float)y, w, h);
            Uint8 *p = px + y * s->pitch + x * 4;
            p[0] = p[1] = p[2] = 255;
            p[3] = (Uint8)(a <= 0 ? 0 : a >= 1 ? 255 : a * 255.0f + 0.5f);
        }
    }
    SDL_Texture *t = SDL_CreateTextureFromSurface(R, s);
    SDL_FreeSurface(s);
    if (t) SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    return t;
}

static float fn_circle(float x, float y, int w, int h)
{
    (void)h;
    float r = w / 2.0f, cov = 0;
    for (int sy = 0; sy < 4; sy++)
        for (int sx = 0; sx < 4; sx++) {
            float dx = x + (sx + 0.5f) / 4 - r, dy = y + (sy + 0.5f) / 4 - r;
            if (dx * dx + dy * dy <= r * r) cov += 1;
        }
    return cov / 16.0f;
}

static float fn_soft(float x, float y, int w, int h)
{
    (void)h;
    float r = w / 2.0f;
    float d = sqrtf((x + 0.5f - r) * (x + 0.5f - r) + (y + 0.5f - r) * (y + 0.5f - r)) / r;
    if (d >= 1) return 0;
    float a = 1 - d;
    return a * a * (3 - 2 * a);
}

#define SH 32 /* shadow falloff size in the 9-slice texture */
static float fn_shadow(float x, float y, int w, int h)
{
    float cx = x + 0.5f, cy = y + 0.5f;
    float dx = cx < SH ? SH - cx : cx > w - SH ? cx - (w - SH) : 0;
    float dy = cy < SH ? SH - cy : cy > h - SH ? cy - (h - SH) : 0;
    float d = sqrtf(dx * dx + dy * dy) / SH;
    if (d >= 1) return 0;
    return expf(-d * d * 4.5f) * (1 - d);
}

/* ------------------------------------------------------------------ primitives */

static float gA = 1; /* global alpha multiplier (used to fade list cards at the edges) */

static void set_draw(Col c)
{
    c.a = (Uint8)(c.a * gA);
    SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(R, c.r, c.g, c.b, c.a);
}

static void fill_rect(float x, float y, float w, float h, Col c)
{
    if (w <= 0 || h <= 0) return;
    set_draw(c);
    SDL_FRect r = {x, y, w, h};
    SDL_RenderFillRectF(R, &r);
}

static void tex_tint(SDL_Texture *t, Col c)
{
    SDL_SetTextureColorMod(t, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(t, (Uint8)(c.a * gA));
}

static void fill_rrect(float x, float y, float w, float h, float r, Col c)
{
    x = floorf(x + 0.5f);
    y = floorf(y + 0.5f);
    w = floorf(w + 0.5f);
    h = floorf(h + 0.5f);
    if (w <= 0 || h <= 0) return;
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    r = floorf(r);
    if (r < 1) {
        fill_rect(x, y, w, h, c);
        return;
    }
    tex_tint(texCircle, c);
    SDL_Rect s;
    SDL_FRect d;
    s = (SDL_Rect){0, 0, 64, 64};   d = (SDL_FRect){x, y, r, r};                 SDL_RenderCopyF(R, texCircle, &s, &d);
    s = (SDL_Rect){64, 0, 64, 64};  d = (SDL_FRect){x + w - r, y, r, r};         SDL_RenderCopyF(R, texCircle, &s, &d);
    s = (SDL_Rect){0, 64, 64, 64};  d = (SDL_FRect){x, y + h - r, r, r};         SDL_RenderCopyF(R, texCircle, &s, &d);
    s = (SDL_Rect){64, 64, 64, 64}; d = (SDL_FRect){x + w - r, y + h - r, r, r}; SDL_RenderCopyF(R, texCircle, &s, &d);
    fill_rect(x + r, y, w - 2 * r, h, c);
    fill_rect(x, y + r, r, h - 2 * r, c);
    fill_rect(x + w - r, y + r, r, h - 2 * r, c);
}

static void disc(float cx, float cy, float rad, Col c)
{
    tex_tint(texCircle, c);
    SDL_FRect d = {cx - rad, cy - rad, rad * 2, rad * 2};
    SDL_RenderCopyF(R, texCircle, NULL, &d);
}

static void glow(float cx, float cy, float rad, Col c, bool additive)
{
    tex_tint(texSoft, c);
    SDL_SetTextureBlendMode(texSoft, additive ? SDL_BLENDMODE_ADD : SDL_BLENDMODE_BLEND);
    SDL_FRect d = {cx - rad, cy - rad, rad * 2, rad * 2};
    SDL_RenderCopyF(R, texSoft, NULL, &d);
}

/* soft shadow / glow around a rectangle; spread = how far it reaches outside */
static void shadow(float x, float y, float w, float h, float spread, Col c)
{
    tex_tint(texShadow, c);
    const int T0 = SH, M = 2; /* source: corners SH x SH, middle 2 px */
    float s = spread;
    float X[4] = {x - s, x - s + s, x + w, x + w + s};
    float Y[4] = {y - s, y, y + h, y + h + s};
    int SX[4] = {0, T0, T0 + M, 2 * T0 + M};
    X[1] = x;
    for (int j = 0; j < 3; j++)
        for (int i = 0; i < 3; i++) {
            SDL_Rect src = {SX[i], SX[j], SX[i + 1] - SX[i], SX[j + 1] - SX[j]};
            SDL_FRect dst = {X[i], Y[j], X[i + 1] - X[i], Y[j + 1] - Y[j]};
            if (dst.w > 0 && dst.h > 0) SDL_RenderCopyF(R, texShadow, &src, &dst);
        }
}

static SDL_Vertex vtx(float x, float y, Col c)
{
    SDL_Vertex v;
    v.position.x = x;
    v.position.y = y;
    v.color.r = c.r;
    v.color.g = c.g;
    v.color.b = c.b;
    v.color.a = (Uint8)(c.a * gA);
    v.tex_coord.x = v.tex_coord.y = 0;
    return v;
}

static void grad_rect(float x, float y, float w, float h, Col tl, Col tr, Col bl, Col br)
{
    if (w <= 0 || h <= 0) return;
    SDL_Vertex v[4] = {vtx(x, y, tl), vtx(x + w, y, tr), vtx(x, y + h, bl), vtx(x + w, y + h, br)};
    int idx[6] = {0, 1, 2, 1, 3, 2};
    SDL_RenderGeometry(R, NULL, v, 4, idx, 6);
}

static void line(float x0, float y0, float x1, float y1, float width, Col c)
{
    float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
    if (len <= 0) return;
    float nx = -dy / len * width / 2, ny = dx / len * width / 2;
    SDL_Vertex v[4] = {vtx(x0 + nx, y0 + ny, c), vtx(x1 + nx, y1 + ny, c), vtx(x0 - nx, y0 - ny, c),
                       vtx(x1 - nx, y1 - ny, c)};
    int idx[6] = {0, 1, 2, 1, 3, 2};
    SDL_RenderGeometry(R, NULL, v, 4, idx, 6);
    disc(x0, y0, width / 2, c);
    disc(x1, y1, width / 2, c);
}

static void triangle(float x0, float y0, Col c0, float x1, float y1, Col c1, float x2, float y2, Col c2)
{
    SDL_Vertex v[3] = {vtx(x0, y0, c0), vtx(x1, y1, c1), vtx(x2, y2, c2)};
    SDL_RenderGeometry(R, NULL, v, 3, NULL, 0);
}

/* rounded rect whose colour runs from c0 (left) to c1 (right), drawn as clipped strips */
static void fill_rrect_hgrad(float x, float y, float w, float h, float r, Col c0, Col c1)
{
    const int strips = 24;
    SDL_Rect old;
    SDL_bool hadClip = SDL_RenderIsClipEnabled(R);
    SDL_RenderGetClipRect(R, &old);
    for (int i = 0; i < strips; i++) {
        int sx0 = (int)floorf(x + w * i / strips), sx1 = (int)floorf(x + w * (i + 1) / strips);
        SDL_Rect strip = {sx0, (int)floorf(y), sx1 - sx0, (int)ceilf(h) + 1};
        SDL_Rect clip = strip;
        if (hadClip && !SDL_IntersectRect(&old, &strip, &clip)) continue;
        SDL_RenderSetClipRect(R, &clip);
        fill_rrect(x, y, w, h, r, mix(c0, c1, (i + 0.5f) / strips));
    }
    SDL_RenderSetClipRect(R, hadClip ? &old : NULL);
}

/* thin line that fades out at both ends */
static void glow_line(float x, float y, float w, Col c)
{
    grad_rect(x, y, w / 2, 1.5f, alpha(c, 0), c, alpha(c, 0), c);
    grad_rect(x + w / 2, y, w / 2, 1.5f, c, alpha(c, 0), c, alpha(c, 0));
}

/* ------------------------------------------------------------------ text */

#define TEXT_CACHE 256
typedef struct {
    unsigned key;
    int font, wrap;
    char *s;
    SDL_Texture *t;
    int w, h;
    unsigned used;
} TextEntry;
static TextEntry cache[TEXT_CACHE];
static unsigned frameNo;

static unsigned hash_str(const char *s, int font, int wrap)
{
    unsigned h = 2166136261u ^ (unsigned)(font * 131 + wrap * 7919);
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

static TextEntry *text_get(int font, const char *s, int wrap)
{
    if (!s || !*s) return NULL;
    unsigned key = hash_str(s, font, wrap);
    TextEntry *slot = &cache[0];
    for (int i = 0; i < TEXT_CACHE; i++) {
        TextEntry *e = &cache[i];
        if (e->t && e->key == key && e->font == font && e->wrap == wrap && !strcmp(e->s, s)) {
            e->used = frameNo;
            return e;
        }
        if (!e->t) slot = e; /* empty slot preferred */
        else if (slot->t && e->used < slot->used) slot = e;
    }
    if (slot->t) {
        SDL_DestroyTexture(slot->t);
        free(slot->s);
        slot->t = NULL;
    }
    SDL_Color white = {255, 255, 255, 255};
    SDL_Surface *surf = wrap > 0 ? TTF_RenderUTF8_Blended_Wrapped(fonts[font], s, white, (Uint32)wrap)
                                 : TTF_RenderUTF8_Blended(fonts[font], s, white);
    if (!surf) return NULL;
    slot->t = SDL_CreateTextureFromSurface(R, surf);
    slot->w = surf->w;
    slot->h = surf->h;
    SDL_FreeSurface(surf);
    if (!slot->t) return NULL;
    SDL_SetTextureBlendMode(slot->t, SDL_BLENDMODE_BLEND);
    slot->s = SDL_strdup(s);
    slot->key = key;
    slot->font = font;
    slot->wrap = wrap;
    slot->used = frameNo;
    return slot;
}

static int text_w(int font, const char *s)
{
    int w = 0, h = 0;
    if (s && *s) TTF_SizeUTF8(fonts[font], s, &w, &h);
    return w;
}

/* Cut a string down to maxw pixels with "..." (respects UTF-8). */
static const char *ellipsize(int font, const char *s, int maxw, char *buf, size_t n)
{
    if (maxw <= 0 || text_w(font, s) <= maxw) return s;
    size_t len = strlen(s);
    if (len + 4 > n) len = n - 4;
    size_t lo = 0, hi = len;
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        while (mid > 0 && ((unsigned char)s[mid] & 0xC0) == 0x80) mid--;
        memcpy(buf, s, mid);
        memcpy(buf + mid, "...", 4);
        if (text_w(font, buf) <= maxw) lo = mid; else hi = mid - 1;
        if (hi < lo) break;
    }
    size_t cut = lo;
    while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) cut--;
    while (cut > 0 && s[cut - 1] == ' ') cut--;
    memcpy(buf, s, cut);
    memcpy(buf + cut, "...", 4);
    return buf;
}

enum { AL_LEFT, AL_CENTER, AL_RIGHT };

/* Draws text with its top at y. Returns the drawn width. */
static int text(int font, const char *s, float x, float y, Col c, int align, int maxw)
{
    char buf[512];
    s = ellipsize(font, s, maxw, buf, sizeof(buf));
    TextEntry *e = text_get(font, s, 0);
    if (!e) return 0;
    float dx = align == AL_CENTER ? x - e->w / 2.0f : align == AL_RIGHT ? x - e->w : x;
    tex_tint(e->t, c);
    SDL_FRect d = {floorf(dx + 0.5f), floorf(y + 0.5f), (float)e->w, (float)e->h};
    SDL_RenderCopyF(R, e->t, NULL, &d);
    return e->w;
}

static int font_h(int font)
{
    return TTF_FontHeight(fonts[font]);
}

/* ------------------------------------------------------------------ widgets */

static Col button_color(const char *b)
{
    if (!strcmp(b, "A")) return C_ACCENT;
    if (!strcmp(b, "B")) return C_RED;
    if (!strcmp(b, "X")) return C_BLUE;
    if (!strcmp(b, "Y")) return C_AMBER;
    return C_TEXT2;
}

static float button_glyph(float x, float cy, const char *b)
{
    float r = 14;
    Col c = button_color(b);
    disc(x + r, cy, r, c);
    disc(x + r, cy, r - 2.5f, RGBA(4, 30, 34, 255));
    disc(x + r - 5, cy - 5, 2.2f, alpha(c, 160)); /* a glint, like a bubble */
    text(F_XS, b, x + r, cy - font_h(F_XS) / 2.0f, c, AL_CENTER, 0);
    return r * 2;
}

static float pill(float right, float cy, const char *label, Col tint, bool active, int icon)
{
    int tw = text_w(F_S, label);
    float iconW = icon ? 26 : 0;
    float w = tw + 32 + iconW, h = 38;
    float x = right - w;
    fill_rrect(x, cy - h / 2, w, h, h / 2, active ? alpha(tint, 40) : RGBA(150, 240, 215, 16));
    float tx = x + 16;
    Col fg = active ? tint : C_TEXT2;
    if (icon == 1) { /* music note */
        float nx = tx + 6, ny = cy + 5;
        disc(nx, ny, 5, fg);
        fill_rect(nx + 3, ny - 15, 2.5f, 15, fg);
        line(nx + 4.5f, ny - 14, nx + 11, ny - 9, 2.5f, fg);
        if (!active) line(tx - 2, cy - 11, tx + 18, cy + 11, 2.5f, alpha(C_RED, 220));
        tx += iconW;
    } else if (icon == 2) { /* SD card */
        float sx = tx, sy = cy - 10;
        fill_rrect(sx, sy, 15, 20, 3, fg);
        fill_rect(sx + 3, sy + 3, 2, 5, RGBA(4, 30, 34, 200));
        fill_rect(sx + 7, sy + 3, 2, 5, RGBA(4, 30, 34, 200));
        tx += iconW;
    }
    text(F_S, label, tx, cy - font_h(F_S) / 2.0f, fg, AL_LEFT, 0);
    return w;
}

static void droplet_logo(float cx, float cy, float s);
static void logo(float cx, float cy, float s)
{
    droplet_logo(cx, cy, s);
    return;
    glow(cx, cy + 4 * s, 46 * s, alpha(C_ACCENT, 60), true);
    /* droplet: circle + triangle */
    float r = 17 * s;
    Col top = C_ACCENT2, bot = RGBA(29, 158, 117, 255);
    triangle(cx, cy - 26 * s, top, cx - r * 0.93f, cy + 1 * s, mix(top, bot, 0.55f), cx + r * 0.93f, cy + 1 * s,
             mix(top, bot, 0.55f));
    disc(cx, cy + 8 * s, r, mix(top, bot, 0.75f));
    /* shine */
    disc(cx - 6 * s, cy + 4 * s, 4.5f * s, RGBA(255, 255, 255, 150));
}

/* ------------------------------------------------------------------ init */

int ui_init(SDL_Renderer *renderer, const void *fontData, size_t fontSize)
{
    R = renderer;
    if (!TTF_WasInit() && TTF_Init() != 0) return -1;
    static const int sizes[F_COUNT] = {15, 21, 27, 36, 40, 68};
    for (int i = 0; i < F_COUNT; i++) {
        SDL_RWops *rw = SDL_RWFromConstMem(fontData, (int)fontSize);
        fonts[i] = TTF_OpenFontRW(rw, 1, sizes[i]);
        if (!fonts[i]) return -2;
        TTF_SetFontHinting(fonts[i], TTF_HINTING_LIGHT);
        TTF_SetFontWrappedAlign(fonts[i], TTF_WRAPPED_ALIGN_CENTER);
    }
    TTF_SetFontStyle(fonts[F_LB], TTF_STYLE_BOLD);
    TTF_SetFontStyle(fonts[F_XL], TTF_STYLE_BOLD);

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    texCircle = make_texture(128, 128, fn_circle);
    texSoft = make_texture(128, 128, fn_soft);
    texShadow = make_texture(2 * SH + 2, 2 * SH + 2, fn_shadow);
    if (!texCircle || !texSoft || !texShadow) return -3;
    return 0;
}

void ui_shutdown(void)
{
    for (int i = 0; i < TEXT_CACHE; i++) {
        if (cache[i].t) SDL_DestroyTexture(cache[i].t);
        free(cache[i].s);
        memset(&cache[i], 0, sizeof(cache[i]));
    }
    if (texCircle) SDL_DestroyTexture(texCircle);
    if (texSoft) SDL_DestroyTexture(texSoft);
    if (texShadow) SDL_DestroyTexture(texShadow);
    texCircle = texSoft = texShadow = NULL;
    for (int i = 0; i < F_COUNT; i++) {
        if (fonts[i]) TTF_CloseFont(fonts[i]);
        fonts[i] = NULL;
    }
}

/* ------------------------------------------------------------------ Tidepool: frame + water */

static float hash01(int i, int salt)
{
    unsigned h = (unsigned)i * 2654435761u ^ (unsigned)salt * 2246822519u;
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    return (h & 0xFFFFFF) / (float)0xFFFFFF;
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

#define WATER_Y 448.0f /* the waterline the shelf stands on */

/* additive quad with per-corner colours (light rays, sheen) */
static void add_quad(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, Col a, Col b)
{
    SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_ADD);
    SDL_Vertex v[4] = {vtx(x0, y0, a), vtx(x1, y1, a), vtx(x2, y2, b), vtx(x3, y3, b)};
    int idx[6] = {0, 1, 2, 1, 3, 2};
    SDL_RenderGeometry(R, NULL, v, 4, idx, 6);
    SDL_SetRenderDrawBlendMode(R, SDL_BLENDMODE_BLEND);
}

static bool gUnderwaterOnly; /* full-screen scenes (progress) sink the waterline off screen */
static bool gLowPower;       /* a transfer is running: draw as little as possible */

void ui_set_low_power(bool on)
{
    gLowPower = on;
}

void ui_begin_frame(double t)
{
    DT = lastT < 0 ? 1.0 / 60 : t - lastT;
    if (DT < 0) DT = 0;
    if (DT > 0.1) DT = 0.1;
    lastT = t;
    T = t;
    frameNo++;
    float tt = (float)t;

    SDL_SetRenderDrawColor(R, 4, 22, 26, 255);
    SDL_RenderClear(R);
    /* sky-to-depth: lighter teal near the surface, near-black deep down */
    if (gUnderwaterOnly) {
        grad_rect(0, 0, UI_W, UI_H, RGBA(6, 34, 40, 255), RGBA(7, 38, 44, 255), RGBA(2, 14, 18, 255), RGBA(2, 14, 18, 255));
    } else {
        grad_rect(0, 0, UI_W, WATER_Y, RGBA(5, 30, 36, 255), RGBA(6, 34, 40, 255), RGBA(9, 52, 58, 255),
                  RGBA(10, 56, 62, 255));
        grad_rect(0, WATER_Y, UI_W, UI_H - WATER_Y, RGBA(6, 40, 46, 255), RGBA(7, 44, 50, 255), RGBA(2, 14, 18, 255),
                  RGBA(2, 14, 18, 255));
    }

    /* sunbeams slanting down through the water, slowly swaying (two during transfers) */
    for (int i = 0; i < (gLowPower ? 2 : 4); i++) {
        float base = 160 + i * 300 + 40 * sinf(tt * 0.21f + i * 1.7f);
        float w = 50 + 40 * hash01(i, 9);
        float sway = 140 + 30 * sinf(tt * 0.15f + i);
        int a = (int)(10 + 8 * (0.5f + 0.5f * sinf(tt * 0.5f + i * 2.1f)));
        add_quad(base, 0, base + w, 0, base + sway - w * 0.6f, UI_H, base + sway + w * 1.4f, UI_H, RGBA(120, 230, 210, a),
                 RGBA(120, 230, 210, 0));
    }
    /* caustic shimmer near the surface (the costliest effect: skipped during transfers) */
    for (int i = 0; i < (gLowPower ? 0 : 4); i++) {
        float x = fmodf(hash01(i, 21) * UI_W + tt * (12 + 10 * hash01(i, 22)), UI_W + 400) - 200;
        float y = 90 + 260 * hash01(i, 23) + 20 * sinf(tt * 0.6f + i);
        glow(x, y, 160 + 90 * hash01(i, 24), RGBA(60, 200, 170, 22), true);
    }
    /* rising bubbles (fewer during transfers) */
    for (int i = 0; i < (gLowPower ? 8 : 22); i++) {
        float speed = 14 + hash01(i, 1) * 30;
        float x = hash01(i, 2) * UI_W + sinf(tt * (0.8f + hash01(i, 6)) + i) * 10;
        float span = UI_H + 40;
        float y = UI_H + 20 - fmodf(tt * speed + hash01(i, 3) * span, span);
        float r = 1.5f + hash01(i, 5) * 3.5f;
        int a = y > WATER_Y ? 70 : 30;
        disc(x, y, r, RGBA(190, 245, 232, a));
    }

    /* the waterline: a bright, gently moving surface with glints */
    if (!gUnderwaterOnly && !gLowPower) {
        glow_line(0, WATER_Y, UI_W, RGBA(140, 240, 215, 150));
        grad_rect(0, WATER_Y - 30, UI_W, 30, RGBA(93, 202, 165, 0), RGBA(93, 202, 165, 0), RGBA(93, 202, 165, 22),
                  RGBA(93, 202, 165, 22));
        for (int i = 0; i < 10; i++) {
            float x = fmodf(hash01(i, 31) * UI_W + tt * (20 + 25 * hash01(i, 32)), UI_W);
            float tw = 0.5f + 0.5f * sinf(tt * (2 + 3 * hash01(i, 33)) + i);
            fill_rect(x, WATER_Y - 0.5f, 6 + 16 * hash01(i, 34), 1.5f, RGBA(220, 255, 245, (int)(40 + 120 * tw)));
        }
    }
    gUnderwaterOnly = false;
}

void ui_end_frame(void)
{
    SDL_RenderPresent(R);
}

void ui_fade(float amount)
{
    if (amount <= 0) return;
    fill_rect(0, 0, UI_W, UI_H, RGBA(1, 10, 12, (int)(amount > 1 ? 255 : amount * 255)));
}

/* ------------------------------------------------------------------ header / footer */

void ui_header(const char *sdText, int music)
{
    float bob = sinf((float)T * 1.4f) * 2;
    logo(54, 46 + bob, 0.82f);
    text(F_LB, "wiidrop", 84, 14, C_TEXT, AL_LEFT, 0);
    text(F_XS, "wii and gamecube, from your server", 87, 66, C_MUTED, AL_LEFT, 0);

    float right = UI_W - 48;
    if (music >= 0) right -= pill(right, 46, music ? "Music" : "Muted", C_ACCENT, music == 1, 1) + 10;
    if (sdText && *sdText) pill(right, 46, sdText, C_ACCENT2, false, 2);
}

void ui_footer(const UiHint *hints, int count, const char *right)
{
    float cy = UI_H - 34;
    grad_rect(0, UI_H - 76, UI_W, 76, RGBA(1, 10, 12, 0), RGBA(1, 10, 12, 0), RGBA(1, 10, 12, 200), RGBA(1, 10, 12, 200));
    float x = 48;
    for (int i = 0; i < count; i++) {
        x += button_glyph(x, cy, hints[i].button) + 9;
        x += text(F_S, hints[i].label, x, cy - font_h(F_S) / 2.0f, C_TEXT2, AL_LEFT, 0) + 28;
    }
    if (right) text(F_S, right, UI_W - 48, cy - font_h(F_S) / 2.0f, C_MUTED, AL_RIGHT, 0);
}

/* ------------------------------------------------------------------ the shelf */

static float approach(float cur, float target, float rate)
{
    return cur + (target - cur) * (1 - expf(-(float)DT * rate));
}

static Col kind_color(UiKind k)
{
    switch (k) {
    case UI_KIND_WII: return RGBA(55, 138, 221, 255);
    case UI_KIND_GC: return RGBA(127, 119, 221, 255);
    case UI_KIND_7Z: return RGBA(186, 117, 23, 255);
    case UI_KIND_ZIP: return RGBA(216, 90, 48, 255);
    default: return RGBA(95, 120, 118, 255);
    }
}

static const char *kind_label(UiKind k)
{
    switch (k) {
    case UI_KIND_WII: return "Wii";
    case UI_KIND_GC: return "GameCube";
    case UI_KIND_7Z: return "7z";
    case UI_KIND_ZIP: return "zip";
    default: return "?";
    }
}

static float chip(float x, float cy, const char *label, Col c) /* left-anchored, returns width */
{
    int tw = text_w(F_XS, label);
    float w = tw + 26, h = 26;
    fill_rrect(x, cy - h / 2, w, h, h / 2, alpha(c, 50));
    disc(x + 11, cy, 3.5f, c);
    text(F_XS, label, x + 19, cy - font_h(F_XS) / 2.0f, c, AL_LEFT, 0);
    return w;
}

static void heart(float cx, float cy, float r, Col c)
{
    disc(cx - r * 0.5f, cy - r * 0.15f, r * 0.55f, c);
    disc(cx + r * 0.5f, cy - r * 0.15f, r * 0.55f, c);
    triangle(cx - r * 1.02f, cy + 0.02f * r, c, cx + r * 1.02f, cy + 0.02f * r, c, cx, cy + r * 0.95f, c);
}

/* Metacritic's own colours: green 75+, yellow 50-74, red below */
static Col meta_color(int score)
{
    return score >= 75 ? RGBA(102, 204, 51, 255) : score >= 50 ? RGBA(255, 204, 51, 255) : RGBA(255, 0, 0, 255);
}

/* a square score badge; returns its width */
static float meta_badge(float x, float cy, int score, float size)
{
    char s[16];
    snprintf(s, sizeof(s), "%d", score);
    Col c = meta_color(score);
    fill_rrect(x, cy - size / 2, size, size, 5, c);
    text(size >= 30 ? F_S : F_XS, s, x + size / 2, cy - font_h(size >= 30 ? F_S : F_XS) / 2.0f,
         score >= 50 && score < 75 ? RGBA(40, 30, 0, 255) : RGBA(255, 255, 255, 255), AL_CENTER, 0);
    return size;
}

/* a box-art card (or a generated one when there's no art yet), standing with its bottom on y */
static void card_face(const UiItem *it, float x, float y, float w, float h, bool sel)
{
    if (it->cover) {
        tex_tint(it->cover, RGBA(255, 255, 255, 255));
        SDL_FRect d = {x, y, w, h};
        SDL_RenderCopyF(R, it->cover, NULL, &d);
    } else {
        Col kc = kind_color(it->kind);
        fill_rrect(x, y, w, h, 8, mix(kc, RGBA(4, 20, 24, 255), 0.55f));
        grad_rect(x + 6, y + 2, w - 12, h * 0.45f, alpha(kc, 90), alpha(kc, 90), alpha(kc, 0), alpha(kc, 0));
        text(F_XS, kind_label(it->kind), x + w / 2, y + 14, alpha(C_TEXT, 200), AL_CENTER, 0);
        TextEntry *te = text_get(sel ? F_S : F_XS, it->title, (int)(w - 22));
        if (te) {
            float th = te->h > h - 70 ? h - 70 : te->h;
            tex_tint(te->t, C_TEXT);
            SDL_Rect src = {0, 0, te->w, (int)th};
            SDL_FRect d = {floorf(x + (w - te->w) / 2), floorf(y + (h - th) / 2 + 8), (float)te->w, th};
            SDL_RenderCopyF(R, te->t, &src, &d);
        }
        logo(x + w / 2, y + h - 30, 0.5f);
    }
}

/* the card's reflection under the waterline, rippling */
static void card_reflection(const UiItem *it, float x, float w, float h)
{
    const int slices = 7;
    float rh = h * 0.5f, sliceH = rh / slices;
    for (int k = 0; k < slices; k++) {
        float off = sinf((float)T * 1.7f + k * 0.65f + x * 0.01f) * (1.0f + k * 0.45f);
        int a = (int)(120 * (1 - (k + 0.5f) / slices) * gA);
        float dy = WATER_Y + 3 + k * sliceH;
        if (it->cover) {
            int tw, th;
            SDL_QueryTexture(it->cover, NULL, NULL, &tw, &th);
            float srcY0 = th - (k + 1) * sliceH * th / h, srcH = sliceH * th / h;
            SDL_Rect src = {0, (int)srcY0, tw, (int)ceilf(srcH)};
            SDL_FRect d = {x + off, dy, w, sliceH + 0.6f};
            SDL_SetTextureColorMod(it->cover, 150, 220, 210);
            SDL_SetTextureAlphaMod(it->cover, (Uint8)a);
            SDL_RenderCopyExF(R, it->cover, &src, &d, 0, NULL, SDL_FLIP_VERTICAL);
        } else {
            Col kc = mix(kind_color(it->kind), RGBA(4, 20, 24, 255), 0.5f);
            fill_rect(x + off + 4, dy, w - 8, sliceH + 0.6f, alpha(kc, a));
        }
    }
}

static float camX = -1;
static int lastCount = -1;
#define MAX_HITS 24
static SDL_FRect hitRect[MAX_HITS];
static int hitIdx[MAX_HITS], nHits;

void ui_forget_covers(void)
{
}

void ui_list(const UiItem *items, int count, int sel)
{
    if (count <= 0) return;
    if (camX < 0 || count != lastCount || fabsf(camX - sel) > 40) camX = (float)sel; /* new list: no fly-by */
    lastCount = count;
    camX = approach(camX, (float)sel, 9);

    const float baseH = 222, selH = 292, step = 186;
    nHits = 0;
    /* draw far cards first so the selected one ends up on top */
    int order[32], n = 0;
    for (int d = 7; d >= 0 && n < 32; d--) {
        int a = (int)floorf(camX) - d, b = (int)floorf(camX) + 1 + d;
        if (d == 0) {
            if (a >= 0 && a < count) order[n++] = a;
            if (b >= 0 && b < count) order[n++] = b;
        } else {
            if (a >= 0 && a < count) order[n++] = a;
            if (b >= 0 && b < count && n < 32) order[n++] = b;
        }
    }
    /* put the selection last */
    for (int k = 0; k < n; k++)
        if (order[k] == sel) {
            int t = order[k];
            memmove(order + k, order + k + 1, (size_t)(n - k - 1) * sizeof(int));
            order[n - 1] = t;
            break;
        }

    for (int k = 0; k < n; k++) {
        int i = order[k];
        const UiItem *it = &items[i];
        float o = i - camX, ao = fabsf(o);
        float near = clampf(1 - ao, 0, 1);
        float h = baseH + (selH - baseH) * near, w = h * 160 / 224.0f;
        float cx = UI_W / 2.0f + o * step + (o > 0 ? 1 : -1) * clampf(ao, 0, 1) * 34;
        bool isSel = i == sel;
        float lift = 22 * near + (isSel ? sinf((float)T * 1.8f) * 4 : 0);
        float x = cx - w / 2, y = WATER_Y - h - lift;
        gA = clampf(1.2f - (ao - 2.6f) / 2.4f, 0, 1) * (isSel ? 1 : 0.86f);
        if (gA <= 0.01f || x > UI_W || x + w < 0) continue;

        card_reflection(it, x, w, h);
        if (isSel) {
            float pulse = 0.5f + 0.5f * sinf((float)T * 2.4f);
            glow(cx, y + h / 2, h * 0.95f, RGBA(93, 202, 165, (int)(40 + 30 * pulse)), true);
            /* ripples spreading on the water where it stands */
            for (int r = 0; r < 3; r++) {
                float ph = fmodf((float)T * 0.6f + r / 3.0f, 1.0f);
                float rw = w * (0.6f + ph * 1.4f);
                tex_tint(texSoft, RGBA(150, 240, 220, (int)(70 * (1 - ph))));
                SDL_SetTextureBlendMode(texSoft, SDL_BLENDMODE_ADD);
                SDL_FRect d = {cx - rw / 2, WATER_Y - 5, rw, 12};
                SDL_RenderCopyF(R, texSoft, NULL, &d);
            }
            fill_rrect(x - 5, y - 5, w + 10, h + 10, 12, RGBA(150, 240, 215, 255));
        } else {
            shadow(x + 6, y + 10, w - 12, h - 12, 16, RGBA(0, 0, 0, 120));
        }
        card_face(it, x, y, w, h, isSel);
        if (!isSel) fill_rect(x, y, w, h, RGBA(4, 24, 28, (int)(70 * (1 - near))));

        /* badges */
        float bx = x + 8, by = y + 18;
        if (it->queuePos > 0) {
            char q[16];
            snprintf(q, sizeof(q), "#%d", it->queuePos);
            int tw = text_w(F_XS, q);
            fill_rrect(bx, by - 12, tw + 16, 24, 12, RGBA(93, 202, 165, 235));
            text(F_XS, q, bx + 8, by - font_h(F_XS) / 2.0f, RGBA(4, 40, 34, 255), AL_LEFT, 0);
            bx += tw + 22;
        }
        if (it->status == UI_ST_INSTALLED) {
            disc(bx + 10, by, 10, RGBA(4, 40, 34, 220));
            line(bx + 5, by, bx + 9, by + 4, 2.4f, C_GREEN);
            line(bx + 9, by + 4, bx + 15, by - 4, 2.4f, C_GREEN);
        } else if (it->status == UI_ST_PARTIAL) {
            disc(bx + 10, by, 10, RGBA(60, 36, 4, 220));
            fill_rect(bx + 6, by - 5, 3, 10, C_AMBER);
            fill_rect(bx + 11, by - 5, 3, 10, C_AMBER);
        }
        if (it->fav) heart(x + w - 16, y + 18, 9, RGBA(240, 120, 140, 255));
        if (it->metacritic > 0) meta_badge(x + w - 32, y + h - 20, it->metacritic, 26);

        if (nHits < MAX_HITS) {
            hitRect[nHits] = (SDL_FRect){x, y, w, h};
            hitIdx[nHits++] = i;
        }
    }
    gA = 1;

    /* the selected game, named under the water */
    const UiItem *s = &items[sel];
    float ty = WATER_Y + 64;
    text(F_LB, s->title ? s->title : "", 80, ty, C_TEXT, AL_LEFT, UI_W - 160);
    float cx = 82, cy = ty + font_h(F_LB) + 20;
    Col kc = kind_color(s->kind);
    cx += chip(cx, cy, kind_label(s->kind), mix(kc, C_TEXT, 0.35f)) + 8;
    if (s->status == UI_ST_INSTALLED) cx += chip(cx, cy, "Installed", C_GREEN) + 8;
    else if (s->status == UI_ST_PARTIAL) cx += chip(cx, cy, "Paused", C_AMBER) + 8;
    if (s->queuePos > 0) {
        char q[24];
        snprintf(q, sizeof(q), "Queue #%d", s->queuePos);
        cx += chip(cx, cy, q, C_ACCENT2) + 8;
    }
    if (s->metacritic > 0) {
        cx += meta_badge(cx + 2, cy, s->metacritic, 28) + 8;
        cx += text(F_XS, "Metacritic", cx, cy - font_h(F_XS) / 2.0f, C_MUTED, AL_LEFT, 0) + 14;
    }
    if (s->subtitle) text(F_S, s->subtitle, cx + 8, cy - font_h(F_S) / 2.0f, C_MUTED, AL_LEFT, (int)(UI_W - 80 - cx - 8));
}

int ui_hit_item(int x, int y, int count)
{
    for (int k = nHits - 1; k >= 0; k--) { /* topmost first */
        SDL_FRect *r = &hitRect[k];
        if (x >= r->x && x <= r->x + r->w && y >= r->y && y <= r->y + r->h && hitIdx[k] < count) return hitIdx[k];
    }
    return -1;
}

void ui_empty(const char *title, const char *body, const char *path)
{
    float cx = UI_W / 2.0f, bob = sinf((float)T * 1.3f) * 8;
    logo(cx, 250 + bob, 2.6f);
    /* its reflection */
    tex_tint(texSoft, RGBA(93, 202, 165, 40));
    SDL_SetTextureBlendMode(texSoft, SDL_BLENDMODE_ADD);
    SDL_FRect d = {cx - 120, WATER_Y - 8, 240, 16};
    SDL_RenderCopyF(R, texSoft, NULL, &d);
    text(F_L, title, cx, WATER_Y + 40, C_TEXT, AL_CENTER, 0);
    TextEntry *e = text_get(F_S, body, 860);
    if (e) {
        tex_tint(e->t, C_TEXT2);
        SDL_FRect r = {floorf(cx - e->w / 2.0f), WATER_Y + 92, (float)e->w, (float)e->h};
        SDL_RenderCopyF(R, e->t, NULL, &r);
    }
    if (path) {
        int w = text_w(F_XS, path);
        float py = WATER_Y + 104 + (e ? e->h : 0) + 10;
        fill_rrect(cx - w / 2.0f - 16, py, w + 32, 32, 16, RGBA(93, 202, 165, 36));
        text(F_XS, path, cx, py + 16 - font_h(F_XS) / 2.0f, C_ACCENT2, AL_CENTER, 0);
    }
}

void ui_search_bar(const char *query, int shown, int total)
{
    const float y = 100, h = 34, x = 48;
    char label[200], count[64];
    snprintf(label, sizeof(label), "%s", query);
    snprintf(count, sizeof(count), "%d of %d", shown, total);
    int lw = text_w(F_S, label);
    if (lw > 560) lw = 560;
    int cw = text_w(F_XS, count);
    float w = 46 + lw + 18 + cw + 18;
    fill_rrect(x, y, w, h, h / 2, RGBA(93, 202, 165, 40));
    float cx = x + 22, cy = y + h / 2 - 1;
    for (int k = 0; k < 18; k++) {
        float a0 = (float)(k * 2 * M_PI / 18), a1 = (float)((k + 1) * 2 * M_PI / 18);
        line(cx + 6.5f * cosf(a0), cy + 6.5f * sinf(a0), cx + 6.5f * cosf(a1), cy + 6.5f * sinf(a1), 2, C_ACCENT2);
    }
    line(cx + 5, cy + 5, cx + 10, cy + 10, 2.6f, C_ACCENT2);
    text(F_S, label, x + 42, y + h / 2 - font_h(F_S) / 2.0f, C_TEXT, AL_LEFT, 560);
    text(F_XS, count, x + w - 16, y + h / 2 - font_h(F_XS) / 2.0f, C_TEXT2, AL_RIGHT, 0);
    int hw = text_w(F_XS, "Clear");
    float hx = x + w + 16;
    button_glyph(hx, y + h / 2, "B");
    text(F_XS, "Clear", hx + 38, y + h / 2 - font_h(F_XS) / 2.0f, C_TEXT2, AL_LEFT, 0);
    (void)hw;
}

/* ------------------------------------------------------------------ progress: a droplet filling up */

static void fmt_bytes(char *out, size_t n, uint64_t b)
{
    if (b >= 1024ULL * 1024 * 1024) snprintf(out, n, "%.2f GB", b / (1024.0 * 1024 * 1024));
    else if (b >= 1024ULL * 1024) snprintf(out, n, "%.1f MB", b / (1024.0 * 1024));
    else snprintf(out, n, "%u KB", (unsigned)(b / 1024));
}

static void check_mark(float cx, float cy, float s, Col c)
{
    line(cx - 7 * s, cy + 0.5f * s, cx - 2 * s, cy + 5.5f * s, 3.2f * s, c);
    line(cx - 2 * s, cy + 5.5f * s, cx + 8 * s, cy - 5.5f * s, 3.2f * s, c);
}

/* droplet outline: tip on top, round bottom (a convex shape), as a polygon */
#define DROP_N 72
static int drop_poly(float cx, float cy, float r, float px[], float py[])
{
    float tipY = cy - r * 2.15f, d = cy - tipY;
    float g = acosf(r / d);                       /* angle at the centre between the tip and a tangent point */
    float thR = (float)M_PI / 2 - g;              /* right tangent point (angles measured with y up) */
    float thL = (float)M_PI / 2 + g - 2 * (float)M_PI; /* left one, reached going clockwise round the bottom */
    int n = 0;
    px[n] = cx;
    py[n++] = tipY;
    for (int k = 0; k <= DROP_N - 2; k++) {
        float th = thR + (thL - thR) * k / (DROP_N - 2);
        px[n] = cx + r * cosf(th);
        py[n++] = cy - r * sinf(th);
    }
    return n;
}

/* clip a convex polygon to y >= level */
static int clip_below(const float *px, const float *py, int n, float level, float *ox, float *oy)
{
    int m = 0;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        bool ai = py[i] >= level, aj = py[j] >= level;
        if (ai) {
            ox[m] = px[i];
            oy[m++] = py[i];
        }
        if (ai != aj) {
            float t = (level - py[i]) / (py[j] - py[i]);
            ox[m] = px[i] + (px[j] - px[i]) * t;
            oy[m++] = level;
        }
    }
    return m;
}

static void fill_convex(const float *px, const float *py, int n, Col top, Col bot, float y0, float y1)
{
    if (n < 3) return;
    SDL_Vertex v[DROP_N * 2 + 4];
    int idx[(DROP_N * 2 + 4) * 3], ni = 0;
    float cxm = 0, cym = 0;
    for (int i = 0; i < n; i++) {
        cxm += px[i];
        cym += py[i];
    }
    cxm /= n;
    cym /= n;
    v[0] = vtx(cxm, cym, mix(top, bot, clampf((cym - y0) / (y1 - y0), 0, 1)));
    for (int i = 0; i < n; i++) v[i + 1] = vtx(px[i], py[i], mix(top, bot, clampf((py[i] - y0) / (y1 - y0), 0, 1)));
    for (int i = 0; i < n; i++) {
        idx[ni++] = 0;
        idx[ni++] = 1 + i;
        idx[ni++] = 1 + (i + 1) % n;
    }
    SDL_RenderGeometry(R, NULL, v, n + 1, idx, ni);
}

static void droplet_logo(float cx, float cy, float s)
{
    float px[DROP_N + 2], py[DROP_N + 2];
    float r = 15 * s, ccy = cy + 6 * s;
    int n = drop_poly(cx, ccy, r, px, py);
    glow(cx, cy + 2 * s, 44 * s, alpha(C_ACCENT, 60), true);
    fill_convex(px, py, n, C_ACCENT2, RGBA(29, 158, 117, 255), ccy - 2.15f * r, ccy + r);
    disc(cx - r * 0.42f, ccy - r * 0.2f, r * 0.2f, RGBA(255, 255, 255, 120));
}

/* a polyline as one triangle strip (one draw call); closed joins the last point to the first */
static void stroke_poly(const float *px, const float *py, int n, float width, Col c, bool closed)
{
    enum { MAXP = 96 };
    if (n < 2 || n > MAXP) return;
    SDL_Vertex v[(MAXP + 1) * 2];
    int idx[(MAXP + 1) * 6], nv = 0, ni = 0, segs = closed ? n : n - 1;
    for (int i = 0; i <= segs; i++) {
        int k = i % n, prev = (k - 1 + n) % n, next = (k + 1) % n;
        if (!closed && i == 0) prev = k;
        if (!closed && i == segs) next = k;
        float dx = px[next] - px[prev], dy = py[next] - py[prev], len = sqrtf(dx * dx + dy * dy);
        if (len <= 0) len = 1;
        float nx = -dy / len * width / 2, ny = dx / len * width / 2;
        v[nv++] = vtx(px[k] + nx, py[k] + ny, c);
        v[nv++] = vtx(px[k] - nx, py[k] - ny, c);
        if (i > 0) {
            int a = nv - 4;
            idx[ni++] = a;
            idx[ni++] = a + 1;
            idx[ni++] = a + 2;
            idx[ni++] = a + 1;
            idx[ni++] = a + 3;
            idx[ni++] = a + 2;
        }
    }
    SDL_RenderGeometry(R, NULL, v, nv, idx, ni);
}

static void draw_droplet(float cx, float cy, float r, float fill, bool known)
{
    float px[DROP_N + 2], py[DROP_N + 2], wx[DROP_N * 2 + 4], wy[DROP_N * 2 + 4];
    int n = drop_poly(cx, cy, r, px, py);
    float top = cy - r * 2.15f, bottom = cy + r;
    glow(cx, cy - r * 0.4f, r * 2.6f, RGBA(93, 202, 165, 36), true);
    fill_convex(px, py, n, RGBA(150, 240, 220, 18), RGBA(93, 202, 165, 30), top, bottom); /* glass */
    float tw = (float)T;

    if (!known) fill = 0.5f + 0.06f * sinf(tw * 1.3f);
    float level = bottom - (bottom - top) * clampf(fill, 0, 1);
    /* waves: lower the level a little at the crest, fill the strip with a sine edge */
    float wave = 6;
    int m = clip_below(px, py, n, level + wave, wx, wy);
    fill_convex(wx, wy, m, RGBA(100, 225, 195, 235), RGBA(20, 110, 100, 245), level, bottom);
    /* the wave strip between level-wave and level+wave, across the drop's width there */
    float xl = cx, xr = cx;
    for (int i = 0; i < m; i++)
        if (fabsf(wy[i] - (level + wave)) < 0.5f) {
            if (wx[i] < xl) xl = wx[i];
            if (wx[i] > xr) xr = wx[i];
        }
    if (xr - xl > 4 && fill > 0.01f) {
        enum { SEG = 32 };
        SDL_Vertex v[(SEG + 1) * 2];
        int idx[SEG * 6], ni = 0;
        float lx[SEG + 1], ly[SEG + 1];
        for (int k = 0; k <= SEG; k++) { /* the wave body and its foam line, one draw call each */
            float wx0 = xl + (xr - xl) * k / SEG;
            float wy0 = level + wave * sinf(tw * 2.6f + wx0 * 0.05f);
            v[k * 2] = vtx(wx0, wy0, RGBA(150, 245, 220, 235));
            v[k * 2 + 1] = vtx(wx0, level + wave + 1, RGBA(100, 225, 195, 235));
            lx[k] = wx0;
            ly[k] = wy0;
            if (k > 0) {
                int a = (k - 1) * 2;
                idx[ni++] = a;
                idx[ni++] = a + 2;
                idx[ni++] = a + 1;
                idx[ni++] = a + 2;
                idx[ni++] = a + 3;
                idx[ni++] = a + 1;
            }
        }
        SDL_RenderGeometry(R, NULL, v, (SEG + 1) * 2, idx, ni);
        stroke_poly(lx, ly, SEG + 1, 2, RGBA(230, 255, 248, 200), false);
        /* bubbles inside the water */
        for (int i = 0; i < (gLowPower ? 4 : 9); i++) {
            float sp = 25 + 30 * hash01(i, 41);
            float span = bottom - level;
            if (span < 20) break;
            float by = bottom - 10 - fmodf((float)T * sp + hash01(i, 42) * span, span - 12);
            float halfW = sqrtf(fmaxf(0, r * r - (by - cy) * (by - cy))) * 0.75f;
            if (by < cy) halfW = (xr - xl) * 0.35f;
            float bx = cx + (hash01(i, 43) * 2 - 1) * halfW + sinf((float)T * 2 + i) * 3;
            disc(bx, by, 2 + 2.5f * hash01(i, 44), RGBA(220, 255, 248, 120));
        }
    }
    /* outline + shine */
    stroke_poly(px, py, n, 3, RGBA(170, 245, 225, 230), true);
    for (int k = 0; k < 9; k++) { /* a curved highlight hugging the left side */
        float ang = (float)M_PI * (0.62f + k * 0.035f);
        disc(cx + r * 0.78f * cosf(ang), cy - r * 0.78f * sinf(ang), 3.2f - k * 0.2f, RGBA(255, 255, 255, 80 - k * 6));
    }
}

void ui_progress(const UiProgress *p)
{
    gUnderwaterOnly = true; /* takes effect next frame: keep the scene calm while working */
    float frac = p->total ? (float)((double)p->done / (double)p->total) : 0;
    bool known = p->total > 0;
    float dcx = 300, dcy = 440, dr = 118;
    draw_droplet(dcx, dcy, dr, frac, known);
    char pct[16];
    if (known) snprintf(pct, sizeof(pct), "%d%%", (int)(frac * 100));
    else snprintf(pct, sizeof(pct), "...");
    text(F_XL, pct, dcx, dcy - font_h(F_XL) / 2.0f - 6, RGBA(255, 255, 255, 255), AL_CENTER, 0);

    float x = 540, y = 150, maxw = UI_W - x - 60;
    /* stages */
    float sx = x;
    for (int i = 0; i < p->stageCount && i < 3; i++) {
        if (!p->stages[i]) continue;
        bool done = i < p->stage, cur = i == p->stage;
        Col c = cur ? C_ACCENT : done ? C_GREEN : C_MUTED;
        float w = text_w(F_XS, p->stages[i]) + 40;
        fill_rrect(sx, y, w, 28, 14, alpha(c, cur ? 60 : 30));
        if (done) check_mark(sx + 15, y + 14, 0.7f, c);
        else disc(sx + 15, y + 14, cur ? 4.5f : 3.5f, c);
        text(F_XS, p->stages[i], sx + 26, y + 14 - font_h(F_XS) / 2.0f, cur ? C_TEXT : c, AL_LEFT, 0);
        sx += w + 10;
    }
    y += 52;
    text(F_L, p->title ? p->title : "", x, y, C_TEXT, AL_LEFT, (int)maxw);
    y += font_h(F_L) + 10;
    if (p->detail) {
        text(F_S, p->detail, x, y, C_TEXT2, AL_LEFT, (int)maxw);
        y += font_h(F_S) + 26;
    }
    /* the bar */
    float bw = maxw, bh = 10;
    fill_rrect(x, y, bw, bh, 5, RGBA(255, 255, 255, 20));
    if (known) {
        float fw = bw * clampf(frac, 0, 1);
        if (fw > 10) fill_rrect_hgrad(x, y, fw, bh, 5, RGBA(60, 180, 150, 255), RGBA(160, 245, 220, 255));
        glow(x + fw, y + bh / 2, 26, RGBA(150, 245, 220, 90), true);
    } else {
        float ph = fmodf((float)T * 0.6f, 1.0f);
        fill_rrect(x + (bw - 140) * ph, y, 140, bh, 5, alpha(C_ACCENT, 200));
    }
    y += bh + 18;
    char a[32], b[32], line1[160];
    if (known) {
        fmt_bytes(a, sizeof(a), p->done);
        fmt_bytes(b, sizeof(b), p->total);
        snprintf(line1, sizeof(line1), "%s of %s", a, b);
    } else if (p->done) {
        fmt_bytes(a, sizeof(a), p->done);
        snprintf(line1, sizeof(line1), "%s", a);
    } else {
        line1[0] = '\0';
    }
    text(F_S, line1, x, y, C_TEXT2, AL_LEFT, 0);
    if (p->bytesPerSec > 0) {
        char sp[48], eta[48] = "";
        fmt_bytes(a, sizeof(a), p->bytesPerSec);
        if (known && p->total > p->done) {
            uint64_t s = (p->total - p->done) / p->bytesPerSec;
            if (s < 60) snprintf(eta, sizeof(eta), "  -  under a minute left");
            else if (s < 3600) snprintf(eta, sizeof(eta), "  -  %u min left", (unsigned)((s + 30) / 60));
            else snprintf(eta, sizeof(eta), "  -  %u h %u min left", (unsigned)(s / 3600), (unsigned)(s % 3600 / 60));
        }
        snprintf(sp, sizeof(sp), "%s/s%s", a, eta);
        text(F_S, sp, x + bw, y, C_ACCENT2, AL_RIGHT, 0);
    }
    if (p->note) text(F_XS, p->note, x, UI_H - 112, C_MUTED, AL_LEFT, (int)maxw);
    if (p->hint) {
        float fy = UI_H - 52;
        button_glyph(x, fy, "B");
        text(F_S, p->hint, x + 40, fy - font_h(F_S) / 2.0f, C_TEXT2, AL_LEFT, 0);
    }
}

/* ------------------------------------------------------------------ message / confirm */

void ui_message(UiMsgKind kind, const char *title, const char *body, const char *button)
{
    unsigned h = hash_str(title ? title : "", 0, 0) ^ hash_str(body ? body : "", 1, 0);
    if (h != msgHash) {
        msgHash = h;
        msgStart = T;
    }
    float a = clampf((float)((T - msgStart) / 0.25), 0, 1);
    float ease = 1 - (1 - a) * (1 - a) * (1 - a);
    fill_rect(0, 0, UI_W, UI_H, RGBA(1, 12, 14, (int)(170 * ease)));

    TextEntry *be = body ? text_get(F_S, body, 640) : NULL;
    float cw = 760, ch = 230 + (be ? be->h : 0);
    float x0 = (UI_W - cw) / 2, y0 = (UI_H - ch) / 2 + (1 - ease) * 40;
    Col kc = kind == UI_MSG_OK ? C_GREEN : kind == UI_MSG_ERROR ? C_RED : C_ACCENT;
    shadow(x0 + 12, y0 + 16, cw - 24, ch - 24, 50, RGBA(0, 0, 0, (int)(180 * ease)));
    fill_rrect(x0, y0, cw, ch, 26, RGBA(6, 36, 42, (int)(248 * ease)));
    glow_line(x0 + 40, y0 + 1, cw - 80, alpha(kc, (int)(230 * ease)));

    /* a droplet icon in the message's colour */
    float icx = UI_W / 2.0f, icy = y0 + 62;
    glow(icx, icy, 70, alpha(kc, (int)(70 * ease)), true);
    float r = 22;
    triangle(icx, icy - 34, alpha(kc, (int)(255 * ease)), icx - r * 0.93f, icy - 4, alpha(kc, (int)(255 * ease)),
             icx + r * 0.93f, icy - 4, alpha(kc, (int)(255 * ease)));
    disc(icx, icy + 4, r, alpha(kc, (int)(255 * ease)));
    Col ink = RGBA(4, 30, 30, (int)(255 * ease));
    if (kind == UI_MSG_OK) check_mark(icx, icy + 5, 1.3f, ink);
    else if (kind == UI_MSG_ERROR) {
        fill_rrect(icx - 2.5f, icy - 10, 5, 16, 2.5f, ink);
        disc(icx, icy + 13, 3, ink);
    } else {
        disc(icx, icy - 8, 3, ink);
        fill_rrect(icx - 2.5f, icy - 2, 5, 17, 2.5f, ink);
    }

    text(F_L, title, icx, y0 + 104, alpha(C_TEXT, (int)(255 * ease)), AL_CENTER, (int)cw - 60);
    if (be) {
        tex_tint(be->t, alpha(C_TEXT2, (int)(255 * ease)));
        SDL_FRect d = {floorf(icx - be->w / 2.0f), y0 + 154, (float)be->w, (float)be->h};
        SDL_RenderCopyF(R, be->t, NULL, &d);
    }
    const char *lbl = button ? button : "OK";
    int tw = text_w(F_S, lbl);
    float fy = y0 + ch - 40;
    if (kind == (UiMsgKind)-1) {
        int cw2 = text_w(F_S, "Cancel");
        float total = 38 + tw + 36 + 38 + cw2, gx = icx - total / 2;
        button_glyph(gx, fy, "A");
        text(F_S, lbl, gx + 38, fy - font_h(F_S) / 2.0f, alpha(C_TEXT, (int)(255 * ease)), AL_LEFT, 0);
        gx += 38 + tw + 36;
        button_glyph(gx, fy, "B");
        text(F_S, "Cancel", gx + 38, fy - font_h(F_S) / 2.0f, alpha(C_TEXT2, (int)(255 * ease)), AL_LEFT, 0);
        return;
    }
    float gx = icx - (38 + tw) / 2.0f;
    button_glyph(gx, fy, "A");
    text(F_S, lbl, gx + 38, fy - font_h(F_S) / 2.0f, alpha(C_TEXT2, (int)(255 * ease)), AL_LEFT, 0);
}

void ui_confirm(const char *title, const char *body, const char *yes)
{
    ui_message((UiMsgKind)-1, title, body, yes);
}

/* ------------------------------------------------------------------ details */

void ui_details(const UiDetails *d)
{
    /* the cover stands on its own patch of water, with a reflection */
    float ch = 318, cw = ch * 160 / 224.0f, cx = 96, bob = sinf((float)T * 1.6f) * 3;
    float cy = WATER_Y - ch - 8 + bob;
    UiItem it = {d->title, NULL, d->kind, d->status, d->cover, d->fav, d->queuePos, d->metacritic};
    card_reflection(&it, cx, cw, ch);
    glow(cx + cw / 2, cy + ch / 2, ch * 0.9f, RGBA(93, 202, 165, 45), true);
    shadow(cx + 8, cy + 12, cw - 16, ch - 16, 26, RGBA(0, 0, 0, 160));
    card_face(&it, cx, cy, cw, ch, true);
    if (d->fav) heart(cx + cw - 20, cy + 22, 12, RGBA(240, 120, 140, 255));

    float x = cx + cw + 80, maxw = UI_W - 70 - x, y = 128;
    shadow(x - 28, y - 20, maxw + 56, 500, 30, RGBA(0, 0, 0, 120));
    fill_rrect(x - 30, y - 22, maxw + 60, 504, 22, RGBA(4, 28, 32, 228));
    glow_line(x, y - 22, maxw, RGBA(150, 240, 215, 160));
    text(F_LB, d->title ? d->title : "", x, y, C_TEXT, AL_LEFT, (int)maxw);
    y += font_h(F_LB) + 16;
    float chx = x, chy = y + 13;
    const char *st = d->status == UI_ST_INSTALLED ? "Installed" : d->status == UI_ST_PARTIAL ? "Partly downloaded"
                                                                                            : "Not downloaded";
    Col sc = d->status == UI_ST_INSTALLED ? C_GREEN : d->status == UI_ST_PARTIAL ? C_AMBER : C_MUTED;
    chx += chip(chx, chy, st, sc) + 8;
    if (d->queuePos > 0) {
        char q[24];
        snprintf(q, sizeof(q), "Queue #%d", d->queuePos);
        chx += chip(chx, chy, q, C_ACCENT2) + 8;
    }
    if (d->metacritic > 0) chx += meta_badge(chx + 4, chy, d->metacritic, 30) + 12;
    y += 52;
    for (int i = 0; i < d->rowCount && i < 8; i++) {
        if (!d->rows[i][0]) continue;
        text(F_XS, d->rows[i][0], x, y + 4, C_MUTED, AL_LEFT, 0);
        text(F_S, d->rows[i][1] ? d->rows[i][1] : "", x + 150, y, C_TEXT, AL_LEFT, (int)(maxw - 150));
        y += 42;
        if (i + 1 < d->rowCount)
            grad_rect(x, y - 10, maxw, 1, RGBA(93, 202, 165, 45), RGBA(93, 202, 165, 0), RGBA(93, 202, 165, 45),
                      RGBA(93, 202, 165, 0));
    }
}

/* ------------------------------------------------------------------ menu: a drawer from the right */

#define MENU_W 540
static float menuY0, menuRow = 46, menuX0 = UI_W - MENU_W;
static unsigned menuFrame;
static double menuOpenT;

void ui_menu(const char *title, const UiMenuItem *items, int count, int sel)
{
    if (frameNo != menuFrame + 1) menuOpenT = T; /* wasn't open last frame: slide in */
    menuFrame = frameNo;
    float a = clampf((float)((T - menuOpenT) / 0.28), 0, 1);
    float ease = 1 - (1 - a) * (1 - a) * (1 - a);
    fill_rect(0, 0, UI_W, UI_H, RGBA(1, 12, 14, (int)(150 * ease)));
    menuX0 = UI_W - MENU_W * ease;
    float x0 = menuX0;
    shadow(x0 + 10, 10, MENU_W, UI_H - 20, 40, RGBA(0, 0, 0, 180));
    grad_rect(x0, 0, MENU_W, UI_H, RGBA(6, 40, 46, 250), RGBA(5, 32, 38, 250), RGBA(3, 20, 24, 252), RGBA(3, 18, 22, 252));
    grad_rect(x0, 0, 2, UI_H, RGBA(150, 240, 215, 0), RGBA(150, 240, 215, 0), RGBA(150, 240, 215, 180),
              RGBA(150, 240, 215, 180));
    logo(x0 + 46, 50, 0.6f);
    text(F_M, title, x0 + 72, 50 - font_h(F_M) / 2.0f, C_TEXT, AL_LEFT, MENU_W - 100);

    menuY0 = 100;
    float avail = UI_H - menuY0 - 74;
    menuRow = count > 0 && avail / count < 46 ? avail / count : 46;
    for (int i = 0; i < count; i++) {
        float ry = menuY0 + i * menuRow, cy = ry + menuRow / 2.0f;
        if (i == sel) {
            fill_rect(x0 + 18, ry + 2, MENU_W - 36, menuRow - 4, RGBA(93, 202, 165, 38));
            fill_rect(x0 + 18, ry + 2, 4, menuRow - 4, C_ACCENT);
        }
        Col lc = items[i].disabled ? C_MUTED : items[i].danger ? C_RED : i == sel ? C_TEXT : C_TEXT2;
        text(F_S, items[i].label, x0 + 40, cy - font_h(F_S) / 2.0f, lc, AL_LEFT, MENU_W - 280);
        if (items[i].value) {
            float vx = x0 + MENU_W - 36;
            bool cyc = i == sel && !items[i].disabled;
            if (cyc) {
                text(F_S, ">", vx, cy - font_h(F_S) / 2.0f, C_ACCENT, AL_RIGHT, 0);
                vx -= 22;
            }
            int vw = text(F_XS, items[i].value, vx, cy - font_h(F_XS) / 2.0f, i == sel ? C_ACCENT2 : C_MUTED, AL_RIGHT, 230);
            if (cyc) text(F_S, "<", vx - vw - 10, cy - font_h(F_S) / 2.0f, C_ACCENT, AL_RIGHT, 0);
        }
    }
    float fy = UI_H - 36, gx = x0 + 40;
    gx += button_glyph(gx, fy, "A") + 9;
    gx += text(F_S, "Select", gx, fy - font_h(F_S) / 2.0f, C_TEXT2, AL_LEFT, 0) + 28;
    gx += button_glyph(gx, fy, "B") + 9;
    text(F_S, "Close", gx, fy - font_h(F_S) / 2.0f, C_TEXT2, AL_LEFT, 0);
}

int ui_menu_hit(int x, int y, int count)
{
    if (x < menuX0 || y < menuY0) return -1;
    int i = (int)((y - menuY0) / menuRow);
    return i < count ? i : -1;
}
