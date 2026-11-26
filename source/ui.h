#pragma once
/*
 * WiiDrop GUI - pure SDL2 + SDL2_ttf, drawn on a 1280x720 canvas (the Wii U port shows the same
 * picture on the TV and the GamePad). No Wii U APIs in here, so it also builds on a PC for previews.
 */
#include <SDL.h>
#include <stdbool.h>
#include <stdint.h>

#define UI_W 1280
#define UI_H 720

typedef enum { UI_KIND_UNKNOWN, UI_KIND_WII, UI_KIND_GC, UI_KIND_7Z, UI_KIND_ZIP } UiKind;
typedef enum { UI_ST_NEW, UI_ST_PARTIAL, UI_ST_INSTALLED } UiStatus;

typedef struct {
    const char *title;
    const char *subtitle;
    UiKind kind;
    UiStatus status;
    SDL_Texture *cover; /* box art, or NULL to show the coloured type tile */
    bool fav;           /* starred */
    int queuePos;       /* 1-based place in the download queue, 0 = not queued */
    int metacritic;     /* Metacritic score 1-100 (from RAWG), 0 = none */
} UiItem;

/* Game details screen. rows: label/value pairs ("Game ID", "RMCE01"). */
typedef struct {
    const char *title;
    SDL_Texture *cover;
    UiKind kind;
    UiStatus status;
    bool fav;
    int queuePos;
    int metacritic;
    const char *rows[8][2];
    int rowCount;
} UiDetails;

typedef struct {
    const char *label;
    const char *value; /* shown on the right (e.g. "A-Z"), or NULL */
    bool danger;       /* red label (Exit, Clear) */
    bool disabled;     /* dimmed */
} UiMenuItem;

typedef struct {
    const char *button; /* "A", "B", "X", "Y", "+" */
    const char *label;
} UiHint;

typedef struct {
    const char *title;        /* game / link name */
    const char *detail;       /* smaller line under the title */
    const char *stages[3];    /* e.g. "Download", "Extract", "Install" */
    int stageCount;
    int stage;                /* index of the current stage */
    uint64_t done, total;     /* bytes; total 0 = unknown */
    uint64_t bytesPerSec;
    const char *hint;         /* footer text next to the B glyph, or NULL */
    const char *note;         /* small muted line, or NULL */
} UiProgress;

typedef enum { UI_MSG_INFO, UI_MSG_OK, UI_MSG_ERROR } UiMsgKind;

/* fontData must stay valid until ui_shutdown(). Returns 0 on success. */
int ui_init(SDL_Renderer *renderer, const void *fontData, size_t fontSize);
void ui_shutdown(void);

/* t = seconds since start; draws the animated background. */
void ui_begin_frame(double t);
void ui_end_frame(void);

/* music: -1 = no music file, 0 = off, 1 = on */
void ui_header(const char *sdText, int music);
void ui_list(const UiItem *items, int count, int sel);
void ui_empty(const char *title, const char *body, const char *path);
/* Shows the active search between the header and the list: the query, "shown of total", "B Clear". */
void ui_search_bar(const char *query, int shown, int total);
void ui_footer(const UiHint *hints, int count, const char *right);
void ui_progress(const UiProgress *p);
/* Modal card over whatever was drawn before. Fades in each time the title/body change. */
void ui_message(UiMsgKind kind, const char *title, const char *body, const char *button);
void ui_details(const UiDetails *d);
/* Modal menu card; ui_menu_hit maps a tap to a row (-1 = outside the card). */
void ui_menu(const char *title, const UiMenuItem *items, int count, int sel);
int ui_menu_hit(int x, int y, int count);
/* Modal yes/no card: "A <yes>" and "B Cancel". */
void ui_confirm(const char *title, const char *body, const char *yes);
/* Fade the whole screen to black (0..1), for exit. */
void ui_fade(float amount);

/* Call before destroying cover textures the GUI may still be showing. */
void ui_forget_covers(void);

/* Index of the list card under a point in canvas coordinates, or -1. */
int ui_hit_item(int x, int y, int count);
