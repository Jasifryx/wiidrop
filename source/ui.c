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

static const Col C_TEXT    = {242, 245, 255, 255};
static const Col C_TEXT2   = {170, 178, 214, 255};
static const Col C_MUTED   = {110, 118, 158, 255};
static const Col C_ACCENT  = {57, 198, 255, 255};  /* cyan */
static const Col C_ACCENT2 = {167, 92, 255, 255};  /* violet */
static const Col C_GREEN   = {61, 220, 151, 255};
static const Col C_AMBER   = {255, 181, 71, 255};
static const Col C_RED     = {255, 92, 122, 255};
static const Col C_BLUE    = {77, 163, 255, 255};
static const Col C_CARD    = {22, 28, 56, 255};

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
static float selY = -1000, scrollY = 0;
static float barShown = 0;
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
    if (!strcmp(b, "A")) return C_GREEN;
    if (!strcmp(b, "B")) return C_RED;
    if (!strcmp(b, "X")) return C_BLUE;
    if (!strcmp(b, "Y")) return C_AMBER;
    return RGBA(150, 158, 196, 255);
}

static float button_glyph(float x, float cy, const char *b)
{
    float r = 15;
    Col c = button_color(b);
    disc(x + r, cy, r + 2, alpha(c, 60));
    disc(x + r, cy, r, c);
    text(F_S, b, x + r, cy - font_h(F_S) / 2.0f - 1, RGBA(12, 16, 34, 255), AL_CENTER, 0);
    return r * 2;
}

static float pill(float right, float cy, const char *label, Col tint, bool active, int icon)
{
    int tw = text_w(F_S, label);
    float iconW = icon ? 26 : 0;
    float w = tw + 32 + iconW, h = 38;
    float x = right - w;
    fill_rrect(x, cy - h / 2, w, h, h / 2, active ? alpha(tint, 46) : RGBA(255, 255, 255, 14));
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
        fill_rect(sx + 3, sy + 3, 2, 5, RGBA(12, 16, 34, 200));
        fill_rect(sx + 7, sy + 3, 2, 5, RGBA(12, 16, 34, 200));
        tx += iconW;
    }
    text(F_S, label, tx, cy - font_h(F_S) / 2.0f, fg, AL_LEFT, 0);
    return w;
}

static void logo(float cx, float cy, float s)
{
    glow(cx, cy + 4 * s, 46 * s, alpha(C_ACCENT, 70), true);
    /* droplet: circle + triangle */
    float r = 17 * s;
    Col top = C_ACCENT, bot = C_ACCENT2;
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

/* ------------------------------------------------------------------ frame + background */

static float hash01(int i, int salt)
{
    unsigned h = (unsigned)i * 2654435761u ^ (unsigned)salt * 2246822519u;
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    return (h & 0xFFFFFF) / (float)0xFFFFFF;
}

void ui_begin_frame(double t)
{
    DT = lastT < 0 ? 1.0 / 60 : t - lastT;
    if (DT < 0) DT = 0;
    if (DT > 0.1) DT = 0.1;
    lastT = t;
    T = t;
    frameNo++;

    SDL_SetRenderDrawColor(R, 8, 11, 26, 255);
    SDL_RenderClear(R);
    grad_rect(0, 0, UI_W, UI_H, RGBA(9, 13, 34, 255), RGBA(18, 12, 44, 255), RGBA(12, 18, 46, 255),
              RGBA(26, 13, 50, 255));

    float tt = (float)t;
    glow(640 + 430 * sinf(tt * 0.13f), 250 + 110 * cosf(tt * 0.17f), 560, RGBA(0, 160, 255, 46), true);
    glow(280 + 190 * cosf(tt * 0.11f), 600 + 80 * sinf(tt * 0.19f), 480, RGBA(170, 60, 255, 50), true);
    glow(1080 + 150 * sinf(tt * 0.09f + 1), 520 + 120 * cosf(tt * 0.12f), 440, RGBA(40, 90, 255, 42), true);

    /* drifting particles */
    for (int i = 0; i < 46; i++) {
        float speed = 8 + hash01(i, 1) * 22;
        float x = hash01(i, 2) * UI_W + sinf(tt * 0.4f + i) * 14;
        float y = UI_H + 20 - fmodf(tt * speed + hash01(i, 3) * (UI_H + 40), UI_H + 40);
        float tw = 0.5f + 0.5f * sinf(tt * (1.2f + hash01(i, 4) * 2) + i);
        float r = 1.2f + hash01(i, 5) * 2.2f;
        disc(x, y, r, RGBA(200, 225, 255, (int)(25 + 70 * tw)));
    }
}

void ui_end_frame(void)
{
    SDL_RenderPresent(R);
}

void ui_fade(float amount)
{
    if (amount <= 0) return;
    fill_rect(0, 0, UI_W, UI_H, RGBA(0, 0, 0, (int)(amount > 1 ? 255 : amount * 255)));
}

/* ------------------------------------------------------------------ header / footer */

void ui_header(const char *sdText, int music)
{
    logo(78, 52, 1.0f);
    text(F_LB, "WiiDrop", 112, 12, C_TEXT, AL_LEFT, 0);
    text(F_S, "Wii & GameCube installer", 115, 68, C_MUTED, AL_LEFT, 0);

    float right = UI_W - 60;
    if (music >= 0) {
        right -= pill(right, 52, music ? "Music" : "Muted", C_ACCENT, music == 1, 1) + 12;
    }
    if (sdText && *sdText) pill(right, 52, sdText, C_GREEN, false, 2);

    /* divider */
    grad_rect(60, 104, (UI_W - 120) / 2.0f, 1, RGBA(255, 255, 255, 0), RGBA(255, 255, 255, 40),
              RGBA(255, 255, 255, 0), RGBA(255, 255, 255, 40));
    grad_rect(UI_W / 2.0f, 104, (UI_W - 120) / 2.0f, 1, RGBA(255, 255, 255, 40), RGBA(255, 255, 255, 0),
              RGBA(255, 255, 255, 40), RGBA(255, 255, 255, 0));
}

void ui_footer(const UiHint *hints, int count, const char *right)
{
    float y = UI_H - 58;
    grad_rect(0, y - 14, UI_W, 72, RGBA(6, 8, 20, 0), RGBA(6, 8, 20, 0), RGBA(6, 8, 20, 190), RGBA(6, 8, 20, 190));
    float x = 60, cy = y + 22;
    for (int i = 0; i < count; i++) {
        x += button_glyph(x, cy, hints[i].button) + 10;
        x += text(F_S, hints[i].label, x, cy - font_h(F_S) / 2.0f, C_TEXT2, AL_LEFT, 0) + 34;
    }
    if (right) text(F_S, right, UI_W - 60, cy - font_h(F_S) / 2.0f, C_MUTED, AL_RIGHT, 0);
}

/* ------------------------------------------------------------------ list */

#define LIST_TOP    150
#define LIST_BOTTOM 650
#define CARD_X      60
#define CARD_W      (UI_W - 120)
#define CARD_H      88
#define CARD_STEP   100
#define CARD_Y0     (LIST_TOP + 14)

static float edge_fade(float y)
{
    const float zone = 70;
    float top = (y + CARD_H - LIST_TOP) / zone, bottom = (LIST_BOTTOM - y) / zone;
    float f = top < bottom ? top : bottom;
    return f < 0 ? 0 : f > 1 ? 1 : f;
}

static float approach(float cur, float target, float rate)
{
    return cur + (target - cur) * (1 - expf(-(float)DT * rate));
}

static Col kind_color(UiKind k)
{
    switch (k) {
    case UI_KIND_WII: return C_BLUE;
    case UI_KIND_GC: return C_ACCENT2;
    case UI_KIND_7Z: return C_AMBER;
    case UI_KIND_ZIP: return RGBA(255, 140, 90, 255);
    default: return RGBA(110, 118, 158, 255);
    }
}

static const char *kind_label(UiKind k)
{
    switch (k) {
    case UI_KIND_WII: return "Wii";
    case UI_KIND_GC: return "GC";
    case UI_KIND_7Z: return "7z";
    case UI_KIND_ZIP: return "zip";
    default: return "?";
    }
}

static float chip(float right, float cy, const char *label, Col c)
{
    int tw = text_w(F_S, label);
    float w = tw + 44, h = 34, x = right - w;
    fill_rrect(x, cy - h / 2, w, h, h / 2, alpha(c, 38));
    disc(x + 18, cy, 4.5f, c);
    text(F_S, label, x + 30, cy - font_h(F_S) / 2.0f, c, AL_LEFT, 0);
    return w;
}

static void star(float cx, float cy, float r, Col c)
{
    float px[10], py[10];
    for (int k = 0; k < 10; k++) {
        float a = (float)(-M_PI / 2 + k * M_PI / 5), rr = (k & 1) ? r * 0.45f : r;
        px[k] = cx + rr * cosf(a);
        py[k] = cy + rr * sinf(a);
    }
    for (int k = 0; k < 10; k++) {
        int n = (k + 1) % 10;
        triangle(cx, cy, c, px[k], py[k], c, px[n], py[n], c);
    }
}

void ui_forget_covers(void)
{
    /* nothing cached any more (the cover backdrop was removed) */
}

void ui_list(const UiItem *items, int count, int sel)
{

    float viewH = (float)(LIST_BOTTOM - CARD_Y0);
    float target = 0;
    float contentH = count * CARD_STEP - (CARD_STEP - CARD_H);
    if (contentH > viewH) {
        float selTop = sel * (float)CARD_STEP;
        target = selTop - viewH / 2 + CARD_H / 2.0f; /* keep selection centred */
        if (target < 0) target = 0;
        if (target > contentH - viewH) target = contentH - viewH;
    }
    scrollY = approach(scrollY, target, 12);
    float wantSelY = CARD_Y0 + sel * (float)CARD_STEP - scrollY;
    if (selY < -500) selY = wantSelY;
    selY = approach(selY, wantSelY, 20);

    SDL_Rect clip = {0, LIST_TOP, UI_W, LIST_BOTTOM - LIST_TOP + 8};
    SDL_RenderSetClipRect(R, &clip);

    /* card backgrounds */
    for (int i = 0; i < count; i++) {
        float y = CARD_Y0 + i * (float)CARD_STEP - scrollY;
        if (y > LIST_BOTTOM || y + CARD_H < LIST_TOP) continue;
        gA = edge_fade(y);
        fill_rrect(CARD_X, y, CARD_W, CARD_H, 20, RGBA(255, 255, 255, 13));
    }
    gA = 1;

    /* moving selection frame */
    if (count > 0) {
        gA = edge_fade(selY);
        float pulse = 0.5f + 0.5f * sinf((float)T * 3.0f);
        shadow(CARD_X + 10, selY + 10, CARD_W - 20, CARD_H - 20, 36, alpha(C_ACCENT, (int)(60 + 50 * pulse)));
        fill_rrect_hgrad(CARD_X - 2, selY - 2, CARD_W + 4, CARD_H + 4, 22, C_ACCENT, C_ACCENT2);
        fill_rrect(CARD_X + 1, selY + 1, CARD_W - 2, CARD_H - 2, 19, C_CARD);
        grad_rect(CARD_X + 20, selY + 1, CARD_W - 40, CARD_H * 0.5f, RGBA(255, 255, 255, 10), RGBA(255, 255, 255, 10),
                  RGBA(255, 255, 255, 0), RGBA(255, 255, 255, 0));
        gA = 1;
    }

    /* contents */
    for (int i = 0; i < count; i++) {
        const UiItem *it = &items[i];
        float y = CARD_Y0 + i * (float)CARD_STEP - scrollY;
        if (y > LIST_BOTTOM || y + CARD_H < LIST_TOP) continue;
        bool isSel = (i == sel);
        float cy = y + CARD_H / 2.0f;
        gA = edge_fade(y);

        /* box art, or a coloured type tile until it arrives */
        Col kc = kind_color(it->kind);
        if (it->cover) {
            float ch = CARD_H - 14, cw = ch * 160 / 224.0f, cx = CARD_X + 46 - cw / 2, cyy = y + 7;
            shadow(cx + 4, cyy + 4, cw - 8, ch - 8, 12, RGBA(0, 0, 0, 150));
            tex_tint(it->cover, RGBA(255, 255, 255, isSel ? 255 : 225));
            SDL_FRect d = {cx, cyy, cw, ch};
            SDL_RenderCopyF(R, it->cover, NULL, &d);
            const char *kl = kind_label(it->kind);
            int bw = text_w(F_XS, kl) + 12;
            fill_rrect(cx + cw - bw + 6, cyy + ch - 16, (float)bw, 20, 6, alpha(kc, 240));
            text(F_XS, kl, cx + cw + 6 - bw / 2.0f, cyy + ch - 16 + 10 - font_h(F_XS) / 2.0f, C_TEXT, AL_CENTER, 0);
        } else {
            fill_rrect(CARD_X + 16, y + 14, 60, 60, 16, alpha(kc, isSel ? 255 : 210));
            grad_rect(CARD_X + 22, y + 15, 48, 26, RGBA(255, 255, 255, 40), RGBA(255, 255, 255, 40),
                      RGBA(255, 255, 255, 0), RGBA(255, 255, 255, 0));
            text(F_M, kind_label(it->kind), CARD_X + 46, cy - font_h(F_M) / 2.0f - 1, RGBA(255, 255, 255, 255),
                 AL_CENTER, 0);
        }

        /* status chip / action */
        float right = CARD_X + CARD_W - 22;
        float used = 0;
        if (it->status == UI_ST_INSTALLED) used = chip(right, cy, "On SD", C_GREEN);
        else if (it->status == UI_ST_PARTIAL) used = chip(right, cy, "Paused", C_AMBER);
        if (it->queuePos > 0) {
            char q[24];
            snprintf(q, sizeof(q), "Queue #%d", it->queuePos);
            used += chip(right - used - (used > 0 ? 10 : 0), cy, q, C_ACCENT) + (used > 0 ? 10 : 0);
        }
        if (isSel) {
            const char *act = "Details";
            float ax = right - used - (used > 0 ? 18 : 0);
            int aw = text_w(F_S, act);
            text(F_S, act, ax, cy - font_h(F_S) / 2.0f, C_TEXT, AL_RIGHT, 0);
            button_glyph(ax - aw - 40, cy, "A");
            used += aw + 40 + 30;
        }

        float tx = CARD_X + 96;
        if (it->fav) {
            star(tx + 11, y + 14 + font_h(F_M) / 2.0f, 11, C_AMBER);
            tx += 28;
        }
        int maxw = (int)(right - used - tx - 24);
        text(F_M, it->title, tx, y + 14, isSel ? C_TEXT : mix(C_TEXT, C_TEXT2, 0.35f), AL_LEFT, maxw);
        if (it->subtitle) text(F_S, it->subtitle, tx + 1, y + 50, C_MUTED, AL_LEFT, maxw);
    }
    gA = 1;
    SDL_RenderSetClipRect(R, NULL);

    /* scrollbar */
    if (contentH > viewH) {
        float trackY = CARD_Y0, trackH = viewH;
        float thumbH = trackH * viewH / contentH;
        if (thumbH < 40) thumbH = 40;
        float thumbY = trackY + (trackH - thumbH) * (scrollY / (contentH - viewH));
        fill_rrect(UI_W - 36, trackY, 4, trackH, 2, RGBA(255, 255, 255, 18));
        fill_rrect(UI_W - 37, thumbY, 6, thumbH, 3, alpha(C_ACCENT, 200));
    }
}

int ui_hit_item(int x, int y, int count)
{
    if (x < CARD_X || x > CARD_X + CARD_W || y < LIST_TOP || y > LIST_BOTTOM) return -1;
    float rel = y - CARD_Y0 + scrollY;
    if (rel < 0) return -1;
    int i = (int)(rel / CARD_STEP);
    if (rel - i * CARD_STEP > CARD_H) return -1; /* in the gap */
    return i < count ? i : -1;
}

void ui_empty(const char *title, const char *body, const char *path)
{
    float cx = UI_W / 2.0f, bob = sinf((float)T * 1.6f) * 6;
    logo(cx, 270 + bob, 2.4f);
    text(F_L, title, cx, 360, C_TEXT, AL_CENTER, 0);
    TextEntry *e = text_get(F_M, body, 900);
    if (e) {
        tex_tint(e->t, C_TEXT2);
        SDL_FRect d = {floorf(cx - e->w / 2.0f), 418, (float)e->w, (float)e->h};
        SDL_RenderCopyF(R, e->t, NULL, &d);
    }
    if (path) {
        int w = text_w(F_S, path);
        float py = 430 + (e ? e->h : 0) + 26;
        fill_rrect(cx - w / 2.0f - 20, py, w + 40, 44, 22, RGBA(255, 255, 255, 18));
        text(F_S, path, cx, py + 22 - font_h(F_S) / 2.0f, C_ACCENT, AL_CENTER, 0);
    }
}

void ui_search_bar(const char *query, int shown, int total)
{
    const float y = 112, h = 34, x = CARD_X;
    char label[200], count[64];
    snprintf(label, sizeof(label), "\"%s\"", query);
    snprintf(count, sizeof(count), "%d of %d", shown, total);
    int lw = text_w(F_S, label);
    if (lw > 620) lw = 620;
    int cw = text_w(F_S, count);
    float w = 16 + 22 + 10 + lw + 22 + cw + 18;
    fill_rrect(x, y, w, h, h / 2, alpha(C_ACCENT, 42));

    /* magnifier */
    float cx = x + 24, cy = y + h / 2 - 2;
    for (int k = 0; k < 18; k++) {
        float a0 = (float)(k * 2 * M_PI / 18), a1 = (float)((k + 1) * 2 * M_PI / 18);
        line(cx + 7 * cosf(a0), cy + 7 * sinf(a0), cx + 7 * cosf(a1), cy + 7 * sinf(a1), 2.2f, C_ACCENT);
    }
    line(cx + 5.5f, cy + 5.5f, cx + 11, cy + 11, 2.8f, C_ACCENT);

    float ty = y + h / 2 - font_h(F_S) / 2.0f;
    text(F_S, label, x + 48, ty, C_TEXT, AL_LEFT, 620);
    text(F_S, count, x + w - 18, ty, C_TEXT2, AL_RIGHT, 0);

    /* "B Clear" on the right */
    int hw = text_w(F_S, "Clear");
    float hx = UI_W - 60 - (30 + 10 + hw);
    button_glyph(hx, y + h / 2, "B");
    text(F_S, "Clear", hx + 40, ty, C_TEXT2, AL_LEFT, 0);
}

/* ------------------------------------------------------------------ progress */

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

void ui_progress(const UiProgress *p)
{
    float cw = 940, ch = 470, cx0 = (UI_W - cw) / 2, cy0 = 140;
    shadow(cx0 + 14, cy0 + 14, cw - 28, ch - 28, 54, RGBA(0, 0, 0, 160));
    fill_rrect(cx0, cy0, cw, ch, 28, RGBA(18, 23, 48, 235));
    glow_line(cx0 + 60, cy0 + 1, cw - 120, alpha(C_ACCENT, 200));

    /* stage steps */
    int n = p->stageCount > 0 ? p->stageCount : 1;
    float gap = 210, sx0 = UI_W / 2.0f - gap * (n - 1) / 2.0f, sy = cy0 + 54;
    for (int i = 0; i < n - 1; i++) {
        Col lc = i < p->stage ? C_GREEN : RGBA(255, 255, 255, 30);
        fill_rect(sx0 + i * gap + 24, sy - 1.5f, gap - 48, 3, lc);
    }
    for (int i = 0; i < n; i++) {
        float x = sx0 + i * gap;
        if (i < p->stage) {
            disc(x, sy, 17, C_GREEN);
            check_mark(x, sy, 1.0f, RGBA(10, 30, 22, 255));
        } else if (i == p->stage) {
            float pulse = 0.5f + 0.5f * sinf((float)T * 4);
            glow(x, sy, 40, alpha(C_ACCENT, (int)(80 + 70 * pulse)), true);
            disc(x, sy, 17, C_ACCENT);
            char num[4];
            snprintf(num, sizeof(num), "%d", i + 1);
            text(F_S, num, x, sy - font_h(F_S) / 2.0f - 1, RGBA(8, 20, 40, 255), AL_CENTER, 0);
        } else {
            disc(x, sy, 17, RGBA(255, 255, 255, 26));
            char num[4];
            snprintf(num, sizeof(num), "%d", i + 1);
            text(F_S, num, x, sy - font_h(F_S) / 2.0f - 1, C_MUTED, AL_CENTER, 0);
        }
        if (p->stages[i])
            text(F_S, p->stages[i], x, sy + 26, i == p->stage ? C_TEXT : C_MUTED, AL_CENTER, 0);
    }

    float midX = UI_W / 2.0f;
    text(F_L, p->title, midX, cy0 + 128, C_TEXT, AL_CENTER, (int)cw - 80);
    if (p->detail) text(F_S, p->detail, midX, cy0 + 176, C_MUTED, AL_CENTER, (int)cw - 80);

    /* numbers */
    float bx = cx0 + 70, bw = cw - 140, by = cy0 + 330;
    char done[32], total[32], line1[96], line2[96];
    fmt_bytes(done, sizeof(done), p->done);
    float pct = 0;
    if (p->total) {
        pct = (float)((double)p->done / (double)p->total);
        if (pct > 1) pct = 1;
        char big[16];
        snprintf(big, sizeof(big), "%d%%", (int)(pct * 100));
        text(F_XL, big, bx - 2, cy0 + 222, C_TEXT, AL_LEFT, 0);
        fmt_bytes(total, sizeof(total), p->total);
        snprintf(line1, sizeof(line1), "%s of %s", done, total);
    } else {
        text(F_XL, done, bx - 2, cy0 + 222, C_TEXT, AL_LEFT, 0);
        snprintf(line1, sizeof(line1), "size unknown");
    }
    line2[0] = '\0';
    if (p->bytesPerSec) {
        char sp[32];
        fmt_bytes(sp, sizeof(sp), p->bytesPerSec);
        if (p->total > p->done) {
            uint64_t eta = (p->total - p->done) / p->bytesPerSec;
            if (eta >= 3600)
                snprintf(line2, sizeof(line2), "%s/s  -  %u:%02u:%02u left", sp, (unsigned)(eta / 3600),
                         (unsigned)(eta / 60 % 60), (unsigned)(eta % 60));
            else
                snprintf(line2, sizeof(line2), "%s/s  -  %u:%02u left", sp, (unsigned)(eta / 60), (unsigned)(eta % 60));
        } else {
            snprintf(line2, sizeof(line2), "%s/s", sp);
        }
    }
    text(F_M, line1, bx + bw, cy0 + 236, C_TEXT2, AL_RIGHT, 0);
    if (line2[0]) text(F_S, line2, bx + bw, cy0 + 276, C_MUTED, AL_RIGHT, 0);

    /* bar */
    float bh = 18;
    fill_rrect(bx, by, bw, bh, bh / 2, RGBA(255, 255, 255, 22));
    if (p->total) {
        barShown = approach(barShown > pct + 0.02f ? pct : barShown, pct, 6);
        float fw = bw * barShown;
        if (fw >= bh) {
            Col endC = mix(C_ACCENT, C_ACCENT2, barShown);
            disc(bx + bh / 2, by + bh / 2, bh / 2, C_ACCENT);
            disc(bx + fw - bh / 2, by + bh / 2, bh / 2, endC);
            grad_rect(bx + bh / 2, by, fw - bh, bh, C_ACCENT, endC, C_ACCENT, endC);
            SDL_Rect clip = {(int)bx, (int)by, (int)fw, (int)bh};
            SDL_RenderSetClipRect(R, &clip);
            float shine = fmodf((float)T * 0.45f, 1.4f) - 0.2f;
            glow(bx + shine * fw, by + bh / 2, 70, RGBA(255, 255, 255, 120), true);
            SDL_RenderSetClipRect(R, NULL);
            glow(bx + fw - bh / 2, by + bh / 2, 34, alpha(endC, 120), true);
        }
    } else { /* indeterminate */
        float seg = 180, pos = (sinf((float)T * 1.8f) * 0.5f + 0.5f) * (bw - seg);
        fill_rrect(bx + pos, by, seg, bh, bh / 2, C_ACCENT);
    }

    /* footer inside card */
    float fy = cy0 + ch - 50;
    if (p->hint) {
        int tw = text_w(F_S, p->hint);
        float total = 30 + 10 + tw;
        float gx = midX - total / 2;
        button_glyph(gx, fy, "B");
        text(F_S, p->hint, gx + 40, fy - font_h(F_S) / 2.0f, C_TEXT2, AL_LEFT, 0);
    }
    if (p->note) text(F_S, p->note, midX, cy0 + ch + 22, C_MUTED, AL_CENTER, 0);
}

/* ------------------------------------------------------------------ message */

void ui_message(UiMsgKind kind, const char *title, const char *body, const char *button)
{
    unsigned h = hash_str(title ? title : "", 0, 0) ^ hash_str(body ? body : "", 1, 0);
    if (h != msgHash) {
        msgHash = h;
        msgStart = T;
    }
    float a = (float)((T - msgStart) / 0.22);
    if (a > 1) a = 1;
    float ease = 1 - (1 - a) * (1 - a) * (1 - a);

    fill_rect(0, 0, UI_W, UI_H, RGBA(4, 6, 18, (int)(175 * ease)));

    TextEntry *be = body ? text_get(F_M, body, 660) : NULL;
    float cw = 780, ch = 250 + (be ? be->h : 0);
    float cx0 = (UI_W - cw) / 2, cy0 = (UI_H - ch) / 2 + (1 - ease) * 30;
    Col kc = kind == UI_MSG_OK ? C_GREEN : kind == UI_MSG_ERROR ? C_RED : C_ACCENT;

    shadow(cx0 + 14, cy0 + 14, cw - 28, ch - 28, 58, RGBA(0, 0, 0, (int)(170 * ease)));
    shadow(cx0 + 18, cy0 + 18, cw - 36, ch - 36, 46, alpha(kc, (int)(45 * ease)));
    fill_rrect(cx0, cy0, cw, ch, 28, RGBA(20, 26, 54, (int)(250 * ease)));
    glow_line(cx0 + 60, cy0 + 1, cw - 120, alpha(kc, (int)(220 * ease)));

    float icx = UI_W / 2.0f, icy = cy0 + 58;
    glow(icx, icy, 70, alpha(kc, (int)(90 * ease)), true);
    disc(icx, icy, 30, alpha(kc, (int)(255 * ease)));
    Col ink = RGBA(12, 16, 34, (int)(255 * ease));
    if (kind == UI_MSG_OK) {
        check_mark(icx, icy, 1.7f, ink);
    } else if (kind == UI_MSG_ERROR) {
        fill_rrect(icx - 3, icy - 16, 6, 21, 3, ink);
        disc(icx, icy + 12, 3.6f, ink);
    } else {
        disc(icx, icy - 13, 3.6f, ink);
        fill_rrect(icx - 3, icy - 5, 6, 21, 3, ink);
    }

    text(F_L, title, icx, cy0 + 104, alpha(C_TEXT, (int)(255 * ease)), AL_CENTER, (int)cw - 60);
    if (be) {
        tex_tint(be->t, alpha(C_TEXT2, (int)(255 * ease)));
        SDL_FRect d = {floorf(icx - be->w / 2.0f), cy0 + 158, (float)be->w, (float)be->h};
        SDL_RenderCopyF(R, be->t, NULL, &d);
    }

    const char *lbl = button ? button : "OK";
    int tw = text_w(F_S, lbl);
    float fy = cy0 + ch - 44;
    if (kind == (UiMsgKind)-1) { /* confirm: A <yes>   B Cancel */
        int cw2 = text_w(F_S, "Cancel");
        float total = 40 + tw + 40 + 40 + cw2, gx = icx - total / 2;
        button_glyph(gx, fy, "A");
        text(F_S, lbl, gx + 40, fy - font_h(F_S) / 2.0f, alpha(C_TEXT, (int)(255 * ease)), AL_LEFT, 0);
        gx += 40 + tw + 40;
        button_glyph(gx, fy, "B");
        text(F_S, "Cancel", gx + 40, fy - font_h(F_S) / 2.0f, alpha(C_TEXT2, (int)(255 * ease)), AL_LEFT, 0);
        return;
    }
    float gx = icx - (30 + 10 + tw) / 2.0f;
    button_glyph(gx, fy, "A");
    text(F_S, lbl, gx + 40, fy - font_h(F_S) / 2.0f, alpha(C_TEXT2, (int)(255 * ease)), AL_LEFT, 0);
}

void ui_confirm(const char *title, const char *body, const char *yes)
{
    ui_message((UiMsgKind)-1, title, body, yes);
}

/* ------------------------------------------------------------------ details */

void ui_details(const UiDetails *d)
{
    const float top = 128;
    /* cover (or a big type tile) on the left */
    float cw = 300, ch = cw * 224 / 160.0f, cx = 90, cy = top + 10;
    Col kc = kind_color(d->kind);
    glow(cx + cw / 2, cy + ch / 2, 260, alpha(kc, 40), true);
    shadow(cx + 10, cy + 12, cw - 20, ch - 20, 40, RGBA(0, 0, 0, 170));
    if (d->cover) {
        SDL_FRect r = {cx, cy, cw, ch};
        tex_tint(d->cover, RGBA(255, 255, 255, 255));
        SDL_RenderCopyF(R, d->cover, NULL, &r);
    } else {
        fill_rrect(cx, cy, cw, ch, 26, alpha(kc, 230));
        grad_rect(cx + 20, cy + 2, cw - 40, ch * 0.4f, RGBA(255, 255, 255, 40), RGBA(255, 255, 255, 40),
                  RGBA(255, 255, 255, 0), RGBA(255, 255, 255, 0));
        text(F_XL, kind_label(d->kind), cx + cw / 2, cy + ch / 2 - font_h(F_XL) / 2.0f, C_TEXT, AL_CENTER, 0);
    }

    /* title + chips */
    float x = cx + cw + 60, maxw = UI_W - 60 - x;
    float tx = x;
    if (d->fav) {
        star(tx + 14, top + 8 + font_h(F_L) / 2.0f, 14, C_AMBER);
        tx += 36;
    }
    float ty = top + 8;
    text(F_L, d->title ? d->title : "", tx, ty, C_TEXT, AL_LEFT, (int)(maxw - (tx - x)));
    ty += font_h(F_L) + 14;
    float chipX = x;
    const char *st = d->status == UI_ST_INSTALLED ? "On SD" : d->status == UI_ST_PARTIAL ? "Partly downloaded"
                                                                                          : "Not downloaded";
    Col sc = d->status == UI_ST_INSTALLED ? C_GREEN : d->status == UI_ST_PARTIAL ? C_AMBER : C_MUTED;
    float w = (float)text_w(F_S, st) + 44;
    chip(chipX + w, ty + 17, st, sc);
    chipX += w + 10;
    if (d->queuePos > 0) {
        char q[24];
        snprintf(q, sizeof(q), "Queue #%d", d->queuePos);
        w = (float)text_w(F_S, q) + 44;
        chip(chipX + w, ty + 17, q, C_ACCENT);
        chipX += w + 10;
    }
    ty += 34 + 26;

    /* info rows */
    for (int i = 0; i < d->rowCount && i < 8; i++) {
        if (!d->rows[i][0]) continue;
        text(F_S, d->rows[i][0], x, ty, C_MUTED, AL_LEFT, 0);
        text(F_M, d->rows[i][1] ? d->rows[i][1] : "", x + 170, ty - 4, C_TEXT2, AL_LEFT, (int)(maxw - 170));
        ty += 46;
        if (i + 1 < d->rowCount)
            fill_rect(x, ty - 12, maxw, 1, RGBA(255, 255, 255, 16));
    }
}

/* ------------------------------------------------------------------ menu */

#define MENU_W   720

static float menuY0, menuRow = 52;

void ui_menu(const char *title, const UiMenuItem *items, int count, int sel)
{
    fill_rect(0, 0, UI_W, UI_H, RGBA(4, 6, 18, 175));
    menuRow = count > 0 && (UI_H - 40 - 166) / (float)count < 52 ? (UI_H - 40 - 166) / (float)count : 52;
    float h = 96 + count * menuRow + 70;
    float x0 = (UI_W - MENU_W) / 2.0f, y0 = (UI_H - h) / 2.0f;
    shadow(x0 + 14, y0 + 14, MENU_W - 28, h - 28, 58, RGBA(0, 0, 0, 170));
    fill_rrect(x0, y0, MENU_W, h, 28, RGBA(20, 26, 54, 250));
    glow_line(x0 + 60, y0 + 1, MENU_W - 120, alpha(C_ACCENT, 220));
    text(F_L, title, UI_W / 2.0f, y0 + 26, C_TEXT, AL_CENTER, 0);
    menuY0 = y0 + 96;
    for (int i = 0; i < count; i++) {
        float ry = menuY0 + i * menuRow, cy = ry + menuRow / 2.0f;
        if (i == sel) {
            fill_rrect_hgrad(x0 + 24, ry + 2, MENU_W - 48, menuRow - 4, 16, alpha(C_ACCENT, 90), alpha(C_ACCENT2, 90));
        }
        Col lc = items[i].disabled ? C_MUTED : items[i].danger ? C_RED : C_TEXT;
        text(F_M, items[i].label, x0 + 48, cy - font_h(F_M) / 2.0f, lc, AL_LEFT, MENU_W - 320);
        if (items[i].value) {
            float vx = x0 + MENU_W - 48;
            if (i == sel && !items[i].disabled) { /* < value > */
                text(F_S, ">", vx, cy - font_h(F_S) / 2.0f, C_ACCENT, AL_RIGHT, 0);
                vx -= 24;
            }
            int vw = text(F_S, items[i].value, vx, cy - font_h(F_S) / 2.0f, i == sel ? C_TEXT : C_TEXT2, AL_RIGHT, 260);
            if (i == sel && !items[i].disabled) text(F_S, "<", vx - vw - 12, cy - font_h(F_S) / 2.0f, C_ACCENT, AL_RIGHT, 0);
        }
    }
    float fy = y0 + h - 40, gx = x0 + 48;
    gx += button_glyph(gx, fy, "A") + 10;
    gx += text(F_S, "Select", gx, fy - font_h(F_S) / 2.0f, C_TEXT2, AL_LEFT, 0) + 34;
    gx += button_glyph(gx, fy, "B") + 10;
    text(F_S, "Close", gx, fy - font_h(F_S) / 2.0f, C_TEXT2, AL_LEFT, 0);
}

int ui_menu_hit(int x, int y, int count)
{
    float x0 = (UI_W - MENU_W) / 2.0f;
    if (x < x0 || x > x0 + MENU_W || y < menuY0) return -1;
    int i = (int)((y - menuY0) / menuRow);
    return i < count ? i : -1;
}
