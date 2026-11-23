#pragma once
/*
 * WiiDrop's background music: an original, minimal dark-ambient piece synthesized live
 * (no audio file, no samples). A low breathing drone with slow, quiet held tones fading in and out
 * over it, through a long dark reverb.
 * It never repeats exactly, but the same seed always gives the same music.
 *
 * Plain C, no platform code: the Wii U app feeds it to SDL_mixer, and tools/bgm_preview.c renders it
 * to a WAV on a PC.
 */
#include <stdint.h>

void bgm_init(int sampleRate, uint32_t seed);
/* Render `frames` stereo frames (interleaved L, R) of 16-bit audio, scaled by gain (0..1). */
void bgm_render(int16_t *out, int frames, float gain);
