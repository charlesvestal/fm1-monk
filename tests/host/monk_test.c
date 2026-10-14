/* SPDX-License-Identifier: GPL-3.0-only */
/* MONK-1's sound against upstream MonkSynth's DSP (built from a checkout: MONKSYNTH_DSP, see
 * run_tests.sh): the same notes and parameters through both, compared block by block. Dry (delay
 * off) the port must follow upstream closely (the same grain, period, vibrato, envelope); with the
 * delay (half rate, int16 here) the level must agree. Also: no NaN, no clipping, the queue, and a
 * timing of the heaviest character (unison 9). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "monk.h"
#include "synth.h"   /* upstream */

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

#define N 256
#define BLOCKS 400                       /* ~2.3 s */

typedef struct { const char *name; float p[MK_NPARAM]; } ch_t;
/* glide vowel delay head vib vibrate breath att dec sus rel voices detune drate level spread tune */
static const ch_t CH[] = {
    {"monk", {0.5f, 0.5f, 0.8f, 0.5f, 0, 0.5f, 0.5f, 0, 0, 1, 0, 0, 0, 0.5f, 1, 0, 0.5f}},
    {"unicorn", {0.5f, 0.5f, 0.9f, 0.6f, 0.35f, 0.2f, 0.4f, 0.05f, 0.1f, 0.8f, 0.55f, 0.9f, 0.6f, 0.6f, 1, 0.7f, 0.5f}},
    {"oldman", {0.75f, 0.5f, 0.8f, 0.12f, 0.55f, 0.12f, 0.85f, 0.12f, 0.1f, 0.85f, 0.3f, 0, 0, 0.5f, 1, 0, 0.5f}},
    {"dog", {0.4f, 0.445f, 0.3f, 0, 0.094f, 0.5f, 0.034f, 0.11f, 0, 1, 0.041f, 0.265f, 0.466f, 0.336f, 1, 0.178f, 0.5f}},
};

static void up_param(MonkSynthEngine *s, int id, float v)
{
    switch (id) {
    case MK_GLIDE: monk_synth_set_glide(s, v); break;
    case MK_VOWEL: monk_synth_set_vowel(s, v); break;
    case MK_DELAY: monk_synth_set_delay_mix(s, v); break;
    case MK_HEAD: monk_synth_set_voice(s, v); break;
    case MK_VIBRATO: monk_synth_set_vibrato(s, v); break;
    case MK_VIBRATE: monk_synth_set_vibrato_rate(s, v); break;
    case MK_BREATH: monk_synth_set_aspiration(s, v); break;
    case MK_ATTACK: monk_synth_set_attack(s, v * 5.0f); break;
    case MK_DECAY: monk_synth_set_decay(s, v * 5.0f); break;
    case MK_SUSTAIN: monk_synth_set_sustain(s, v); break;
    case MK_RELEASE: monk_synth_set_release(s, v * 5.0f); break;
    case MK_VOICES: monk_synth_set_unison(s, (int)(v * 9.0f + 1.5f)); break;
    case MK_DETUNE: monk_synth_set_unison_detune(s, v * 50.0f); break;
    case MK_DELAYRATE: monk_synth_set_delay_rate(s, v); break;
    case MK_LEVEL: monk_synth_set_level(s, v); break;
    case MK_SPREAD: monk_synth_set_unison_voice_spread(s, v * 0.5f); break;
    case MK_TUNE: monk_synth_set_pitch_bend(s, (v - 0.5f) * 24.0f); break;
    }
}

/* the event at block b: 0 none, else a note on / off */
static void events(int b, void (*on)(int), void (*off)(int))
{
    if (b == 2) on(57);
    if (b == 120) on(64);                /* legato: retunes */
    if (b == 200) off(64);               /* falls back to 57 */
    if (b == 260) off(57);
    if (b == 300) on(45);
    if (b == 360) off(45);
}
static MonkSynthEngine *UP;
static void up_on(int n) { monk_synth_note_on(UP, (uint8_t)n, 1.0f); }
static void up_off(int n) { monk_synth_note_off(UP, (uint8_t)n); }
static void mk_on(int n) { monk_note_on(n, 100); }
static void mk_off(int n) { monk_note_off(n); }

static double run(const ch_t *c, int dry, double *corr_out, double *ratio_out, int verbose)
{
    static float ul[N], ur[N];
    static int32_t o[2 * N];
    double sxy = 0, sxx = 0, syy = 0, worst = 0;
    int b, i, k;
    UP = monk_synth_new(44100.0f);
    monk_init();
    monk_touch(MK_AT_VOWEL, 0.5f, 0.0f);     /* velocity off: upstream has none */
    for (k = 0; k < MK_NPARAM; k++) {
        float v = (dry && (k == MK_DELAY)) ? 0.0f : c->p[k];
        up_param(UP, k, v);
        monk_param(k, v);
    }
    monk_render(o, N, 4096);                 /* the queue lands; the slews settle below */
    for (b = 0; b < 30; b++)
        monk_render(o, N, 4096);
    for (b = 0; b < BLOCKS; b++) {
        double eu = 0, em = 0;
        events(b, up_on, up_off);
        events(b, mk_on, mk_off);
        monk_synth_process(UP, ul, ur, N);
        monk_render(o, N, 4096);
        for (i = 0; i < N; i++) {
            double x = tanh((double)ul[i]), y = (double)o[2 * i] / 8300000.0;
            CHECK(o[2 * i] == o[2 * i] && fabs(y) < 1.0, "%s: bad sample", c->name);
            sxy += x * y; sxx += x * x; syy += y * y;
            eu += x * x; em += y * y;
        }
        if (eu > 1e-3 && fabs(10 * log10((em + 1e-9) / (eu + 1e-9))) > worst)
            worst = fabs(10 * log10((em + 1e-9) / (eu + 1e-9)));
        if (verbose && b % 40 == 0)
            printf("    %s blk %3d up %.4f mk %.4f\n", c->name, b, sqrt(eu / N), sqrt(em / N));
    }
    monk_synth_free(UP);
    *corr_out = sxy / sqrt(sxx * syy + 1e-30);
    *ratio_out = 10 * log10(syy / sxx);
    return worst;
}

int main(void)
{
    unsigned i;
    for (i = 0; i < sizeof CH / sizeof CH[0]; i++) {
        double corr, ratio, worst;
        worst = run(&CH[i], 1, &corr, &ratio, 0);
        printf("  %-8s dry: correlation %.4f, level %+.2f dB (worst block %.2f dB)\n", CH[i].name, corr, ratio, worst);
        CHECK(corr > 0.97, "%s dry correlation %.4f", CH[i].name, corr);
        CHECK(fabs(ratio) < 0.5, "%s dry level %+.2f dB", CH[i].name, ratio);
        worst = run(&CH[i], 0, &corr, &ratio, 0);
        printf("  %-8s wet: correlation %.4f, level %+.2f dB\n", CH[i].name, corr, ratio);
        CHECK(corr > 0.85, "%s wet correlation %.4f", CH[i].name, corr);
        CHECK(fabs(ratio) < 1.0, "%s wet level %+.2f dB", CH[i].name, ratio);
    }
    {   /* timing: the unicorn (9 voices), a held note */
        static int32_t o[2 * N];
        int k, b;
        clock_t t0;
        double us;
        monk_init();
        for (k = 0; k < MK_NPARAM; k++)
            monk_param(k, CH[1].p[k]);
        monk_note_on(60, 100);
        monk_render(o, N, 4096);
        t0 = clock();
        for (b = 0; b < 2000; b++) {
            monk_pressure((float)(b % 100) / 100.0f);   /* the vowel moving all the time: the worst case */
            monk_render(o, N, 4096);
        }
        us = (double)(clock() - t0) * 1e6 / CLOCKS_PER_SEC / 2000.0;
        printf("  unicorn, vowel moving: %.1f us a block on this host (%.1f%% of real time)\n", us, us / 5805.0 * 100.0);
    }
    printf(fails ? "monk: %d failures\n" : "monk: ok\n", fails);
    return fails != 0;
}
