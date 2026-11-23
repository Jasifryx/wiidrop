#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *CONFIG_TEMPLATE =
    "# WiiDrop settings (all optional)\n"
    "\n"
    "# HTTP basic auth, sent with every link in links.txt\n"
    "user =\n"
    "password =\n"
    "\n"
    "# HTTPS certificates are checked against the list of trusted authorities built into WiiDrop.\n"
    "# For your own server with a self-signed certificate: put its certificate (or your own CA) on the SD card\n"
    "# and set ca_file = fs:/vol/external01/wiiu/apps/wiidrop/myserver.pem, or set insecure = 1 to skip the\n"
    "# check (the connection is still encrypted, but anyone on your network could pretend to be your server).\n"
    "ca_file =\n"
    "insecure = 0\n"
    "\n"
    "# Menu music: 1 = on, 0 = start muted (Y toggles it in the app).\n"
    "# Put your own track at sd:/wiiu/apps/wiidrop/music.mp3 to replace the built-in one.\n"
    "music = 1\n"
    "\n"
    "# Downloads use several connections at once (much faster on the Wii U). Set 1 for servers that\n"
    "# only allow one connection.\n"
    "connections = 4\n"
    "\n"
    "# Optional: load links.txt from your server instead of the SD card (the SD copy is the backup).\n"
    "links_url =\n"
    "\n"
    "# Preferred region when 'Hide region duplicates' is on: USA, Europe or Japan.\n"
    "region = USA\n";

static const char *LINKS_TEMPLATE =
    "# WiiDrop links - one direct download link per line.\n"
    "# Optionally put a name in front, separated by |\n"
    "#\n"
    "#   Mario Kart Wii | https://example.com/my-dumps/RMCE01.wbfs\n"
    "#   https://example.com/my-dumps/Melee.iso\n"
    "#\n"
    "# Wii: .wbfs, .iso or .rvz (converted to WBFS automatically). GameCube: .iso or .rvz.\n"
    "# A .7z or .zip containing any of those works too.\n"
    "# The link has to give the file itself, not a web page about it.\n";

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}

static void write_template(const char *path, const char *text)
{
    FILE *t = fopen(path, "w");
    if (t) {
        fputs(text, t);
        fclose(t);
    }
}

void config_load(const char *path, Config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->insecure = 0; /* certificates are checked against the built-in list (data/cacert.pem) */
    cfg->music = 1;
    cfg->connections = 4;
    snprintf(cfg->region, sizeof(cfg->region), "USA");

    FILE *f = fopen(path, "r");
    if (!f) {
        write_template(path, CONFIG_TEMPLATE);
        return;
    }

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        if (*s == '#' || *s == ';' || *s == '\0') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim(s);
        char *val = trim(eq + 1);

        if (!strcmp(key, "user")) snprintf(cfg->user, sizeof(cfg->user), "%s", val);
        else if (!strcmp(key, "password")) snprintf(cfg->password, sizeof(cfg->password), "%s", val);
        else if (!strcmp(key, "ca_file")) snprintf(cfg->ca_file, sizeof(cfg->ca_file), "%s", val);
        else if (!strcmp(key, "insecure")) cfg->insecure = atoi(val) != 0;
        else if (!strcmp(key, "music")) cfg->music = atoi(val) != 0;
        else if (!strcmp(key, "connections")) {
            cfg->connections = atoi(val);
            if (cfg->connections < 1) cfg->connections = 1;
            if (cfg->connections > 8) cfg->connections = 8;
        } else if (!strcmp(key, "links_url")) snprintf(cfg->links_url, sizeof(cfg->links_url), "%s", val);
        else if (!strcmp(key, "region") && *val) snprintf(cfg->region, sizeof(cfg->region), "%s", val);
    }
    fclose(f);
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* "https://host/a/My%20Game.wbfs?dl=1" -> "My Game.wbfs" */
static void name_from_url(const char *url, char *out, size_t n)
{
    const char *end = url + strcspn(url, "?#");
    const char *start = end;
    while (start > url && start[-1] != '/') start--;
    if (start == end) { /* URL ends in '/' */
        snprintf(out, n, "%.*s", (int)(end - url), url);
        return;
    }
    size_t j = 0;
    for (const char *p = start; p < end && j + 1 < n; p++) {
        if (*p == '%' && p + 2 < end && hexval(p[1]) >= 0 && hexval(p[2]) >= 0) {
            out[j++] = (char)(hexval(p[1]) * 16 + hexval(p[2]));
            p += 2;
        } else if (*p == '+') {
            out[j++] = ' ';
        } else {
            out[j++] = *p;
        }
    }
    out[j] = '\0';
}

int links_load(const char *path, Link **out)
{
    *out = NULL;
    FILE *f = fopen(path, "r");
    if (!f) {
        write_template(path, LINKS_TEMPLATE);
        return -1;
    }

    Link *links = NULL;
    int count = 0, cap = 0;
    char line[1400];
    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        if (*s == '#' || *s == '\0') continue;

        /* "URL", "Name | URL", "Name | URL | md5=...", "URL | sha1=..." */
        char *fields[3] = {s, NULL, NULL};
        int nf = 1;
        for (char *bar; nf < 3 && (bar = strchr(fields[nf - 1], '|')) != NULL; nf++) {
            *bar = '\0';
            fields[nf] = bar + 1;
        }
        for (int k = 0; k < nf; k++) fields[k] = trim(fields[k]);
        char *name = NULL, *url = fields[0], *sum = NULL;
        if (nf >= 2 && !strncmp(fields[0], "http", 4) && strstr(fields[0], "://")) {
            sum = fields[1];
        } else if (nf >= 2) {
            name = fields[0];
            url = fields[1];
            if (nf == 3) sum = fields[2];
        }
        if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) continue;

        if (count == cap) {
            cap = cap ? cap * 2 : 32;
            Link *nl = realloc(links, (size_t)cap * sizeof(Link));
            if (!nl) break;
            links = nl;
        }
        Link *l = &links[count++];
        memset(l, 0, sizeof(*l));
        snprintf(l->url, sizeof(l->url), "%s", url);
        if (sum) {
            char *eq = strchr(sum, '=');
            if (eq) {
                *eq = '\0';
                char *kind = trim(sum), *hex = trim(eq + 1);
                for (char *c = kind; *c; c++) *c = (char)tolower((unsigned char)*c);
                for (char *c = hex; *c; c++) *c = (char)tolower((unsigned char)*c);
                size_t want = !strcmp(kind, "md5") ? 32 : !strcmp(kind, "sha1") ? 40 : !strcmp(kind, "crc32") ? 8 : 0;
                if (want && strlen(hex) == want && strspn(hex, "0123456789abcdef") == want) {
                    snprintf(l->sumType, sizeof(l->sumType), "%s", kind);
                    snprintf(l->sum, sizeof(l->sum), "%s", hex);
                }
            }
        }
        l->named = (name && *name) ? 1 : 0;
        if (name && *name) snprintf(l->name, sizeof(l->name), "%s", name);
        else name_from_url(url, l->name, sizeof(l->name));
    }
    fclose(f);
    *out = links;
    return count;
}
