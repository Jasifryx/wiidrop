#pragma once

/* Optional settings from config.ini. Everything has a sensible default, so the file may be missing. */
typedef struct {
    char user[128];     /* optional HTTP basic auth, sent to every link */
    char password[128];
    char ca_file[256];  /* optional CA bundle path for https */
    int insecure;       /* 1 = skip TLS certificate checks */
    int music;          /* 1 = play menu music (default), 0 = start muted */
    int connections;    /* parallel connections per download (1-8, default 4) */
    char links_url[1024]; /* optional: fetch links.txt from here (the SD copy is the fallback) */
    char region[16];    /* preferred region for hiding duplicates: USA / Europe / Japan */
} Config;

/* Loads config.ini, writing a commented template if it does not exist. */
void config_load(const char *path, Config *cfg);

typedef struct {
    char name[128]; /* label shown in the list (from "Name | URL" or the URL's file name) */
    char url[1024];
    int named;      /* 1 = name came from "Name | URL" */
    char sumType[8]; /* optional checksum of the downloaded file: "md5", "sha1" or "crc32" ("" = none) */
    char sum[41];    /* lowercase hex */
} Link;

/* Parses links.txt: one link per line, optionally "Name | URL". '#' starts a comment.
   Returns the number of links (malloc'd array in *out), or -1 if the file is missing
   (a template is then written). */
int links_load(const char *path, Link **out);
