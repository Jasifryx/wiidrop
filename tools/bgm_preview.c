/* Renders WiiDrop's generated background music (source/bgm.c) to a WAV file, to listen to it on a PC.
   Build: cl /O2 /I..\source bgm_preview.c ..\source\bgm.c     (or: cc -O2 -I../source bgm_preview.c ../source/bgm.c -lm)
   Use:   bgm_preview out.wav [seconds] [seed] */
#define _CRT_SECURE_NO_WARNINGS
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "bgm.h"

static void le32(FILE *f, unsigned v)
{
    unsigned char b[4] = {(unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16), (unsigned char)(v >> 24)};
    fwrite(b, 1, 4, f);
}

static void le16(FILE *f, unsigned v)
{
    unsigned char b[2] = {(unsigned char)v, (unsigned char)(v >> 8)};
    fwrite(b, 1, 2, f);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: bgm_preview out.wav [seconds] [seed]\n");
        return 1;
    }
    int secs = argc > 2 ? atoi(argv[2]) : 120;
    unsigned seed = argc > 3 ? (unsigned)strtoul(argv[3], NULL, 0) : 0x57696944u;
    const int rate = 48000;
    FILE *f = fopen(argv[1], "wb");
    if (!f) return 1;
    unsigned frames = (unsigned)secs * rate, bytes = frames * 4;
    fwrite("RIFF", 1, 4, f);
    le32(f, 36 + bytes);
    fwrite("WAVEfmt ", 1, 8, f);
    le32(f, 16);
    le16(f, 1);
    le16(f, 2);
    le32(f, rate);
    le32(f, rate * 4);
    le16(f, 4);
    le16(f, 16);
    fwrite("data", 1, 4, f);
    le32(f, bytes);

    bgm_init(rate, seed);
    static short buf[4800 * 2];
    double sumSq = 0;
    int peak = 0;
    for (unsigned done = 0; done < frames; done += 4800) {
        int n = frames - done < 4800 ? (int)(frames - done) : 4800;
        bgm_render(buf, n, 1.0f);
        for (int i = 0; i < n * 2; i++) {
            int v = abs(buf[i]);
            if (v > peak) peak = v;
            sumSq += (double)buf[i] * buf[i];
        }
        fwrite(buf, 2, (size_t)n * 2, f);
    }
    fclose(f);
    double rms = sqrt(sumSq / (frames * 2.0));
    printf("%d s, peak %.1f dBFS, rms %.1f dBFS\n", secs, 20 * log10(peak / 32768.0), 20 * log10(rms / 32768.0));
    return 0;
}
