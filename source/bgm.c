#include "bgm.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------------ basics */

#define TAB_BITS 12
#define TAB_SIZE (1 << TAB_BITS)
static float sinTab[TAB_SIZE + 1];

static float rate = 48000;
static uint32_t rng = 1;
static uint64_t t; /* samples since start */

static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static float rndf(void) /* 0..1 */
{
    return (rnd() >> 8) * (1.0f / 16777216.0f);
}

/* phase in cycles (0..1) */
static float osc(float ph)
{
    float x = ph * TAB_SIZE;
    int i = (int)x;
    float f = x - i;
    return sinTab[i] + (sinTab[i + 1] - sinTab[i]) * f;
}

/* advance a phase and keep it in 0..1 (an ever-growing float would lose pitch accuracy within minutes) */
#define ADV(ph, inc) do { (ph) += (inc); (ph) -= (float)(int)(ph); } while (0)

static float mtof(float m)
{
    return 440.0f * powf(2.0f, (m - 69.0f) / 12.0f);
}

static uint64_t secs(float s)
{
    return (uint64_t)(s * rate);
}

/* ------------------------------------------------------------------ voices */

/* drone: low D and its octave, breathing slowly */
static float dronePh1, dronePh2, droneLfo;

/* held tones: fade in over several seconds, stay a while, fade out; two detuned copies, one per side */
#define TONES 4
typedef struct {
    int stage; /* 0 off, 1 fading in, 2 holding, 3 fading out */
    float freq, phA, phB, amp, peak, step, lfo, lfoRate;
    uint64_t holdUntil;
} Tone;
static Tone tones[TONES];
static uint64_t nextToneAt;
static int lastNote;

/* D minor, around the drone: A2 C3 D3 F3 A3 C4 D4 (D and A twice as likely) */
static const int notes[] = {45, 45, 48, 50, 50, 53, 57, 57, 60, 62};
#define NNOTES ((int)(sizeof(notes) / sizeof(notes[0])))

static void start_tone(void)
{
    int sounding = 0;
    for (int i = 0; i < TONES; i++) sounding += tones[i].stage != 0;
    if (sounding >= 2) return; /* never more than two at once */
    Tone *v = NULL;
    for (int i = 0; i < TONES && !v; i++)
        if (!tones[i].stage) v = &tones[i];
    if (!v) return;
    int n;
    do n = notes[rnd() % NNOTES];
    while (n == lastNote);
    lastNote = n;
    v->stage = 1;
    v->freq = mtof((float)n);
    v->phA = rndf();
    v->phB = rndf();
    v->amp = 0;
    v->peak = (n < 50 ? 0.050f : 0.036f) * (0.8f + 0.4f * rndf());
    v->step = v->peak / (7.0f * rate); /* 7 s fade in */
    v->lfo = rndf();
    v->lfoRate = 0.08f + rndf() * 0.08f;
    v->holdUntil = 0;
}

/* ------------------------------------------------------------------ reverb */

/* Freeverb-style: 4 combs + 2 allpasses per side */
#define NCOMB 4
#define NAP 2
static const int combLen[NCOMB] = {1557, 1617, 1491, 1422};
static const int apLen[NAP] = {556, 441};
static float combBuf[2][NCOMB][2400], combStore[2][NCOMB];
static int combN[2][NCOMB], combPos[2][NCOMB];
static float apBuf[2][NAP][900];
static int apN[2][NAP], apPos[2][NAP];

static float reverb(int ch, float in)
{
    float out = 0;
    for (int i = 0; i < NCOMB; i++) {
        float *b = combBuf[ch][i];
        int p = combPos[ch][i];
        float y = b[p];
        combStore[ch][i] = y * 0.6f + combStore[ch][i] * 0.4f; /* dark */
        b[p] = in + combStore[ch][i] * 0.88f;                  /* long tail */
        combPos[ch][i] = (p + 1) % combN[ch][i];
        out += y;
    }
    for (int i = 0; i < NAP; i++) {
        float *b = apBuf[ch][i];
        int p = apPos[ch][i];
        float bo = b[p];
        b[p] = out + bo * 0.5f;
        out = bo - out;
        apPos[ch][i] = (p + 1) % apN[ch][i];
    }
    return out * 0.25f;
}

/* ------------------------------------------------------------------ API */

void bgm_init(int sampleRate, uint32_t seed)
{
    rate = (float)sampleRate;
    rng = seed ? seed : 0x9E3779B9u;
    t = 0;
    for (int i = 0; i <= TAB_SIZE; i++) sinTab[i] = (float)sin(2 * M_PI * i / TAB_SIZE);
    memset(tones, 0, sizeof(tones));
    memset(combBuf, 0, sizeof(combBuf));
    memset(combStore, 0, sizeof(combStore));
    memset(apBuf, 0, sizeof(apBuf));
    float sc = rate / 44100.0f;
    for (int ch = 0; ch < 2; ch++) {
        for (int i = 0; i < NCOMB; i++) {
            combN[ch][i] = (int)((combLen[i] + ch * 23) * sc);
            if (combN[ch][i] > 2400) combN[ch][i] = 2400;
            combPos[ch][i] = 0;
        }
        for (int i = 0; i < NAP; i++) {
            apN[ch][i] = (int)((apLen[i] + ch * 23) * sc);
            if (apN[ch][i] > 900) apN[ch][i] = 900;
            apPos[ch][i] = 0;
        }
    }
    dronePh1 = dronePh2 = droneLfo = 0;
    nextToneAt = secs(3.0f);
    lastNote = 0;
}

void bgm_render(int16_t *out, int frames, float gain)
{
    const float inv = 1.0f / rate;
    for (int f = 0; f < frames; f++, t++) {
        if (t >= nextToneAt) {
            start_tone();
            nextToneAt = t + secs(7.0f + rndf() * 6.0f);
        }

        /* drone */
        ADV(droneLfo, 0.03f * inv);
        float breathe = 0.7f + 0.3f * osc(droneLfo);
        ADV(dronePh1, 36.71f * inv);
        ADV(dronePh2, 73.42f * 1.0015f * inv);
        float drone = (osc(dronePh1) * 0.15f + osc(dronePh2) * 0.05f) * breathe;
        float L = drone, R = drone;

        /* held tones */
        float tl = 0, tr = 0;
        for (int i = 0; i < TONES; i++) {
            Tone *v = &tones[i];
            if (!v->stage) continue;
            if (v->stage == 1) {
                v->amp += v->step;
                if (v->amp >= v->peak) {
                    v->amp = v->peak;
                    v->stage = 2;
                    v->holdUntil = t + secs(5.0f + rndf() * 6.0f);
                }
            } else if (v->stage == 2) {
                if (t >= v->holdUntil) {
                    v->stage = 3;
                    v->step = v->peak / (9.0f * rate); /* 9 s fade out */
                }
            } else {
                v->amp -= v->step;
                if (v->amp <= 0) {
                    v->stage = 0;
                    continue;
                }
            }
            ADV(v->lfo, v->lfoRate * inv);
            float a = v->amp * (0.85f + 0.15f * osc(v->lfo)); /* slow shimmer */
            ADV(v->phA, v->freq * 0.998f * inv);
            ADV(v->phB, v->freq * 1.002f * inv);
            float pa = v->phA * 2, pb = v->phB * 2;
            pa -= (float)(int)pa;
            pb -= (float)(int)pb;
            tl += (osc(v->phA) + 0.18f * osc(pa)) * a;
            tr += (osc(v->phB) + 0.18f * osc(pb)) * a;
        }
        L += tl * 0.7f;
        R += tr * 0.7f;
        float wet = (tl + tr) * 0.35f + drone * 0.05f;

        /* reverb, then a gentle limiter */
        L += reverb(0, wet);
        R += reverb(1, wet);
        L *= gain;
        R *= gain;
        L = L / (1.0f + fabsf(L) * 0.6f);
        R = R / (1.0f + fabsf(R) * 0.6f);
        int li = (int)(L * 30000.0f), ri = (int)(R * 30000.0f);
        out[f * 2] = (int16_t)(li > 32767 ? 32767 : li < -32768 ? -32768 : li);
        out[f * 2 + 1] = (int16_t)(ri > 32767 ? 32767 : ri < -32768 ? -32768 : ri);
    }
}
