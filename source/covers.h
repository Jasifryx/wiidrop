#pragma once
/*
 * Background cover-art worker.
 *
 * For each requested link it (1) works out the game ID if it isn't known yet, by asking the link's
 * server for just the first bytes of the file (the disc header: ID + title; RVZ keeps it up front
 * too), and (2) downloads the box art for that ID from GameTDB (art.gametdb.com) into a cache folder
 * on the SD card. Runs on its own thread so the GUI never freezes.
 *
 * Built for big lists (10,000+ links): the app only requests the links near what's on screen, the
 * ID cache is a hash table, and the cover folder is scanned once instead of checked per game.
 */
#include "config.h"

enum { PEEK_NONE = 0, PEEK_RAW = 1, PEEK_ZIP = 2 };

typedef struct {
    int index;      /* the caller's entry index (returned unchanged) */
    char hash[9];   /* link key (same as installed.txt) */
    char url[1024];
    int peek;       /* how to ask the server for the disc header: PEEK_NONE / PEEK_RAW / PEEK_ZIP */
    char name[128]; /* file name (e.g. "Mario Kart Wii (USA).zip"), for the GameTDB title lookup */
    char altName[128]; /* the user's name for the link, tried second */
    char id[7];     /* known ID, or filled in by the worker */
    char title[65]; /* disc title from the header, if peeked */
    int hasCover;   /* cover PNG is in the cache */
    int retry;      /* failed for a reason that may go away (network): ask again later */
    int metacritic; /* filled by the worker: Metacritic score 1-100, 0 = none, -1 = not looked up */
    int userRating; /* RAWG user rating x10 (0-50), 0 = none */
    char why[120];  /* why there's no ID / cover, for the details screen */
} CoverJob;

/* cacheDir: where <ID>.png files go. loaderRoot/loaderDir: optional extra copy into a loader's cover
   folder (e.g. USB Loader GX's images/2D), made only if loaderRoot exists. Both may be NULL. */
void covers_start(const Config *userCfg, const char *cacheDir, const char *loaderRoot, const char *loaderDir,
                  const char *idCachePath);
void covers_stop(void);

/* Replace the pending requests (in priority order). A request already being worked on finishes
   and is still reported. */
void covers_request(const CoverJob *jobs, int count);

/* Finished requests since the last call (max n). Returns how many. */
int covers_poll(CoverJob *out, int n);

/* While paused the worker starts no new requests (used during big downloads). */
void covers_pause(int paused);

void covers_path(const char *id, char *out, unsigned n);

/* 1 if <id>.png is in the cover cache (no SD access: uses the startup scan + new downloads). */
int covers_have(const char *id);

/* Look up an ID/title cached from an earlier header peek. Returns 1 if found. */
int covers_cached_id(const char *hash, char id[7], char title[65]);

/* Title -> ID lookup against GameTDB's title list (downloaded once into the cover folder as
   titles.txt). Exposed for tests; the worker uses it when the file itself can't be peeked. */
int covers_load_titles(const char *path);

/* Scores from RAWG, cached in the cover folder (scores.txt). 1 if this ID was looked up already
   (metacritic 0 then means "no score"). */
int covers_score(const char *id, int *metacritic, int *userRating);
/* Pull name / metacritic / rating for each game out of a RAWG /api/games reply, and pick the one whose
   name matches `title`. Exposed for tests. 1 if a game matched. */
int covers_parse_rawg(const char *json, const char *title, int *metacritic, int *userRating);
int covers_id_from_name(const char *fileName, char id[7], char title[65]);
