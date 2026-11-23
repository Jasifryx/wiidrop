#pragma once

#include <stddef.h>
#include <stdint.h>

#include "config.h"

typedef struct {
    /* Called once before the first body bytes. resumed = 1 if the server honoured the
       Range request (HTTP 206), 0 if it is sending the whole file from byte 0. length is the number
       of bytes the server is about to send (0 if unknown). Return 0 to continue. */
    int (*begin)(void *ud, int resumed, uint64_t length);
    /* Return 0 on success, nonzero to abort. */
    int (*write)(void *ud, const void *data, size_t len);
    /* done/total include resume_from. Return nonzero to cancel. */
    int (*progress)(void *ud, uint64_t done, uint64_t total, uint64_t bytes_per_sec);
    void *ud;
} DownloadSink;

int net_init(void);
void net_shutdown(void);
/* Re-join the network after a drop (Wii U: ACConnect). */
void net_reconnect(void);

/* Downloads a small text file into a malloc'd, NUL-terminated buffer. NULL on error (err filled). */
char *net_fetch_text(const Config *cfg, const char *url, char *err, size_t errlen);

/* Small binary fetch into a malloc'd buffer of at most `max` bytes (for disc headers and cover art).
   rangeOnly = ask for bytes 0..max-1 and stop once that many arrived, even if the server ignores
   the Range header. Gives up if *cancel becomes nonzero. NULL on error. */
uint8_t *net_fetch_mem(const Config *cfg, const char *url, size_t max, int rangeOnly, volatile int *cancel,
                       size_t *outLen, char *err, size_t errlen);

/* What the current / last download is doing: "4 connections", or why it uses only one. */
const char *net_status(void);

/* Size of the file at url (HEAD request), 0 if unknown. */
uint64_t net_fetch_size(const Config *cfg, const char *url);

/* Streams url into sink, asking the server to start at resume_from. 0 on success,
   1 if cancelled by the user, -1 on error (err filled). Big files are fetched over
   cfg->connections connections at once (pieces still reach the sink in order). */
int net_download(const Config *cfg, const char *url, uint64_t resume_from,
                 DownloadSink *sink, char *err, size_t errlen);
