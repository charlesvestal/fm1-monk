/* SPDX-License-Identifier: MIT AND GPL-3.0-only
 *
 * MonkSynth's DSP (dsp/voice.c, synth.c, delay.c), Copyright (c) 2026 Jonathan Taylor, MIT License:
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
 *   associated documentation files (the "Software"), to deal in the Software without restriction,
 *   including without limitation the rights to use, copy, modify, merge, publish, distribute,
 *   sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions: The above copyright notice and this
 *   permission notice shall be included in all copies or substantial portions of the Software.
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
 *
 * Ported to the FM-1 for MONK-1 (GPL-3.0-only as a whole). What changed, and why:
 *   - memory: upstream's voice is 168 KB (its tables, sized for 192 kHz, in every voice). The tables
 *     are shared here and sized for 44.1 kHz; the overlap buffer is 2048 (a grain lands at most a
 *     block ahead of the read, and is 882 long); the delay runs at half rate in int16 (70 KB).
 *   - speed: the grain is recomputed only when what it is made of changes (vowel, head size,
 *     breath); upstream rebuilt the same grain every pitch period. The decay and the window are
 *     multiplied into one table per formant. The pitch's 2^(k/384) comes from a table and is
 *     looked up only when the quantized pitch step changes (upstream: a powf every sample).
 *   - pitch in note units throughout (no hz <-> note round trips: no libm here), and velocity:
 *     upstream ignores it; here it scales the level by `vel_sens`.
 *   - the wrapper of the Schwung port: knob and pressure slewed per block, pressure added to the
 *     vowel or bending the pitch.
 * The sound otherwise follows upstream step for step: the grain, the 32-steps-a-semitone period,
 * the vibrato with its jitter, the two glides, the ADSR, the unison spread, the delay's taps. */
#include "monk.h"
#include "fastmath.h"

#ifdef OM_HOST
#define MK_POOL
#else
#define MK_POOL __attribute__((section(".pool")))
#endif

#define NUM_FORMANTS 3
#define SPLINE_SIZE 1280
#define SPLINE_SEG 320
#define SINE_SIZE 1024
#define GRAIN_LEN 882                 /* 0.02 s at 44.1 kHz */
#define OLAP 2048u                    /* > a block (256) + a grain */
#define MAX_UNISON 10
#define MAX_NOTES 16
#define BLOCK 256u
#define PITCH_TABLE_BASE 8.175799f    /* MIDI note 0 */
#define ENV_ANTICLICK 0.003f
#define RAMP_TICKS 10

static const float FORMANT_FREQS[NUM_FORMANTS][5] = {
    {280.0f, 450.0f, 800.0f, 350.0f, 270.0f},
    {600.0f, 800.0f, 1150.0f, 2000.0f, 2140.0f},
    {2240.0f, 2830.0f, 2900.0f, 2800.0f, 2950.0f},
};
static const float FORMANT_BW[NUM_FORMANTS] = {32.5f, 47.5f, 62.5f};

/* ------------------------------------------------------------ the shared tables */
static float sine_tbl[SINE_SIZE] MK_POOL;
static float formant_tbl[NUM_FORMANTS][SPLINE_SIZE] MK_POOL;
static float env_tbl[NUM_FORMANTS][GRAIN_LEN] MK_POOL;   /* the formant's decay times the window */
static float asp_tbl[GRAIN_LEN] MK_POOL;                 /* the aspiration times the window */
static float p2_tbl[384];                                /* 2^(k / 384) */

enum { ENV_IDLE, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };

typedef struct {
    uint8_t active;
    float current_pitch, target_pitch;   /* note units */
    float pitch_bend_offset;
    float glide_param, min_glide;
    float pitch_step;
    int pitch_ramp_ticks;
    float vowel_step;
    int vowel_ramp_ticks;
    int ramp_counter, ramp_period;
    float vibrato_phase, vibrato_depth, vibrato_rate, rate_scale;
    float random_jitter;
    uint32_t jitter_counter, jitter_period;
    uint32_t rng_state;
    uint32_t overlap_write_pos, overlap_read_pos, overlap_offset;
    float current_vowel, target_vowel, current_voice, aspiration_amp;
    uint8_t grain_dirty;
    float g_vowel, g_voice, g_asp;       /* what the grain in grain[] was made of (-1: none yet) */
    int last_step;                       /* the period's quantized pitch step, and its period */
    uint32_t last_period;
    int env_stage;
    float env_level, env_attack, env_decay, env_sustain, env_release;
    float overlap_buf[OLAP];
    float grain[GRAIN_LEN];
} mk_voice_t;

typedef struct {
    int16_t l[17664], r[17664];          /* half rate: the longest tap (rate 1) is 17570 */
    uint32_t w;
    float target_l, target_r, cur_l, cur_r;
    float coeff, rate, feedback, mix;
    float tap_l, tap_r;                  /* the last half-rate taps (the upsampling's other end) */
} mk_delay_t;
#define DL_N 17664u
#define DL_SCALE 1024.0f                 /* int16 holds +-32 */

static struct {
    mk_voice_t v[MAX_UNISON];
    int unison_count;
    float unison_detune, unison_voice_spread, base_voice, last_base_note;
    uint8_t held[MAX_NOTES], held_count;
    float cc_volume, level, cur_gain, target_gain;
} S MK_POOL;
static mk_delay_t D MK_POOL;

volatile float monk_ui_vowel = 0.5f, monk_ui_amp;
volatile uint8_t monk_ui_active;

static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
static float clamp01(float v) { return clampf(v, 0.0f, 1.0f); }

/* ------------------------------------------------------------------- tables */
static void build_tables(void)
{
    int f, seg, i;
    for (i = 0; i < SINE_SIZE; i++)
        sine_tbl[i] = fm_sinf(FM_TWO_PI * (float)i / SINE_SIZE);
    for (f = 0; f < NUM_FORMANTS; f++) {         /* Catmull-Rom across the five vowels */
        const float *pts = FORMANT_FREQS[f];
        float padded[7] = {pts[0], pts[0], pts[1], pts[2], pts[3], pts[4], pts[4]};
        for (seg = 0; seg < 4; seg++) {
            float p0 = padded[seg], p1 = padded[seg + 1], p2 = padded[seg + 2], p3 = padded[seg + 3];
            for (i = 0; i < SPLINE_SEG; i++) {
                float t = (float)i / SPLINE_SEG, t2 = t * t, t3 = t2 * t;
                formant_tbl[f][seg * SPLINE_SEG + i] =
                    0.5f * ((2.0f * p1) + (-p0 + p2) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                            (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
            }
        }
    }
    {   /* the grain window: a short cosine attack, flat, a cosine release that stops short of zero */
        float sr = MK_FS, bw = 50.0f * FM_PI, p1 = sr * 0.000202f, p2 = sr * 0.000263f;
        uint32_t attack_len = (uint32_t)(sr * 0.0018f), release_start = (uint32_t)(sr * 0.013f);
        uint32_t release_len = (uint32_t)(sr * 0.007f), decay_len = GRAIN_LEN * 4u, n;
        for (n = 0; n < GRAIN_LEN; n++) {
            float w = 1.0f, s1, s2, d1 = 0.0f, d2 = 0.0f;
            uint32_t i1, i2;
            if (n < attack_len)
                w = 0.5f * (1.0f - fm_cosf(FM_PI * (float)n / (float)attack_len));
            if (n >= release_start)
                w = 0.5f * (1.0f - fm_cosf(FM_PI * (float)(release_len + n) / (float)release_len));
            for (f = 0; f < NUM_FORMANTS; f++) {
                uint32_t di = (uint32_t)(FORMANT_BW[f] * (float)n / 50.0f);
                env_tbl[f][n] = (di < decay_len ? fm_expf(-bw * (float)di / sr) : 0.0f) * w;
            }
            s1 = fm_sinf(FM_TWO_PI * (float)n / p1);   /* the breath: two inharmonic decaying sines */
            i1 = (uint32_t)((float)n * 3.0f);
            if (i1 < decay_len)
                d1 = fm_expf(-bw * (float)i1 / sr);
            s2 = fm_sinf(FM_TWO_PI * (float)n / p2);
            i2 = (uint32_t)((float)n * 3.5f);
            if (i2 < decay_len)
                d2 = fm_expf(-bw * (float)i2 / sr);
            asp_tbl[n] = (s1 * d1 + s2 * d2) * w;
        }
    }
    for (i = 0; i < 384; i++)
        p2_tbl[i] = fm_exp2f((float)i / 384.0f);
}

/* ------------------------------------------------------------------- voice */
static float next_rand(mk_voice_t *v)
{
    v->rng_state = v->rng_state * 1103515245u + 12345u;
    return (float)((v->rng_state >> 16) & 0x7FFF) / 32768.0f;
}

static float env_tick(mk_voice_t *v)
{
    float rate;
    switch (v->env_stage) {
    case ENV_ATTACK:
        rate = 1.0f / (fm_maxf(v->env_attack, ENV_ANTICLICK) * MK_FS);
        v->env_level += rate;
        if (v->env_level >= 1.0f) {
            v->env_level = 1.0f;
            v->env_stage = v->env_decay > 0.0f ? ENV_DECAY : ENV_SUSTAIN;
        }
        break;
    case ENV_DECAY:
        rate = v->env_decay > 0.0f ? 1.0f / (v->env_decay * MK_FS) : 1.0f;
        v->env_level -= rate * (1.0f - v->env_sustain);
        if (v->env_level <= v->env_sustain) {
            v->env_level = v->env_sustain;
            v->env_stage = ENV_SUSTAIN;
        }
        break;
    case ENV_RELEASE:
        rate = 1.0f / (fm_maxf(v->env_release, ENV_ANTICLICK) * MK_FS);
        v->env_level -= rate;
        if (v->env_level <= 0.0f) {
            v->env_level = 0.0f;
            v->env_stage = ENV_IDLE;
            v->active = 0;
        }
        break;
    default:
        break;
    }
    return v->env_level;
}

static float lookup_formant(int f, float vowel)
{
    float pos = clampf(vowel, 0.0f, 1.0f) * (SPLINE_SIZE - 1), frac;
    int idx = (int)pos;
    if (idx > SPLINE_SIZE - 2)
        idx = SPLINE_SIZE - 2;
    frac = pos - (float)idx;
    return formant_tbl[f][idx] * (1.0f - frac) + formant_tbl[f][idx + 1] * frac;
}

/* the grain period: the pitch quantized to 32 steps a semitone (audible, and intended) */
static uint32_t grain_period(mk_voice_t *v, float note)
{
    int step = (int)((note - 12.0f) * 32.0f), oct, k;
    float freq;
    if (step == v->last_step)
        return v->last_period;
    v->last_step = step;
    oct = step >= 0 ? step / 384 : -((-step + 383) / 384);
    k = step - oct * 384;
    freq = p2_tbl[k] * PITCH_TABLE_BASE;
    freq = oct >= 0 ? freq * (float)(1u << (oct > 20 ? 20 : oct)) : freq / (float)(1u << (-oct > 20 ? 20 : -oct));
    v->last_period = (uint32_t)(MK_FS / freq);
    return v->last_period;
}

static void apply_portamento(mk_voice_t *v)
{
    if (v->min_glide > 0.001f) {
        if (--v->ramp_counter > 0)
            return;
        v->ramp_counter = v->ramp_period;
        if (v->pitch_ramp_ticks > 0) {
            v->pitch_ramp_ticks--;
            v->current_pitch += v->pitch_step;
            if (v->pitch_ramp_ticks == 0)
                v->current_pitch = v->target_pitch;
        }
        if (v->vowel_ramp_ticks > 0) {
            v->vowel_ramp_ticks--;
            v->current_vowel += v->vowel_step;
            if (v->vowel_ramp_ticks == 0)
                v->current_vowel = v->target_vowel;
        }
    } else if (v->glide_param > 0.001f) {
        float diff = v->target_pitch - v->current_pitch;
        float rate = 12.0f / ((v->glide_param + 0.01f) * MK_FS);
        if (diff > 0.2f)
            v->current_pitch += rate;
        else if (diff < -0.2f)
            v->current_pitch -= rate;
        else
            v->current_pitch = v->target_pitch;
        v->current_vowel = v->target_vowel;
    } else {
        v->current_pitch = v->target_pitch;
        v->current_vowel = v->target_vowel;
    }
}

static float compute_vibrato(mk_voice_t *v)
{
    float rate = (v->vibrato_depth * 0.2f + 1.0f) * v->random_jitter * v->rate_scale;
    v->vibrato_phase += rate * ((float)SINE_SIZE / MK_FS);
    while (v->vibrato_phase >= (float)SINE_SIZE)
        v->vibrato_phase -= (float)SINE_SIZE;
    if (++v->jitter_counter >= v->jitter_period) {
        v->jitter_counter = 0;
        v->random_jitter = next_rand(v) * 2.0f + 5.0f;
    }
    return (v->vibrato_depth + 0.2f) * sine_tbl[(int)v->vibrato_phase % SINE_SIZE];
}

static void compute_grain(mk_voice_t *v)
{
    float fs = v->current_voice * 0.5f + 0.75f, k[NUM_FORMANTS], asp = v->aspiration_amp;
    const float *e0 = env_tbl[0], *e1 = env_tbl[1], *e2 = env_tbl[2];
    float *g = v->grain;
    int f;
    uint32_t i;
    for (f = 0; f < NUM_FORMANTS; f++)          /* sine-table steps per sample */
        k[f] = lookup_formant(f, v->current_vowel) * fs * ((float)SINE_SIZE / MK_FS);
    for (i = 0; i < GRAIN_LEN; i++) {
        float fi = (float)i;
        g[i] = sine_tbl[(uint32_t)(fi * k[0]) & (SINE_SIZE - 1)] * e0[i] +
               sine_tbl[(uint32_t)(fi * k[1]) & (SINE_SIZE - 1)] * e1[i] +
               sine_tbl[(uint32_t)(fi * k[2]) & (SINE_SIZE - 1)] * e2[i] + asp_tbl[i] * asp;
    }
    v->g_vowel = v->current_vowel;
    v->g_voice = v->current_voice;
    v->g_asp = asp;
    v->grain_dirty = 0;
}

static void overlap_add(mk_voice_t *v)
{
    uint32_t s, i, n1;
    v->overlap_write_pos = (v->overlap_write_pos + v->overlap_offset) & (OLAP - 1u);
    s = v->overlap_write_pos;
    n1 = OLAP - s < GRAIN_LEN ? OLAP - s : GRAIN_LEN;
    for (i = 0; i < n1; i++)
        v->overlap_buf[s + i] += v->grain[i];
    for (; i < GRAIN_LEN; i++)
        v->overlap_buf[i - n1] += v->grain[i];
}

static void voice_init(mk_voice_t *v, int i)
{
    float *ob = v->overlap_buf;
    uint32_t k;
    for (k = 0; k < OLAP; k++)
        ob[k] = 0.0f;
    v->active = 0;
    v->current_pitch = v->target_pitch = 57.0f;   /* 220 Hz */
    v->pitch_bend_offset = 0.0f;
    v->glide_param = v->min_glide = 0.0f;
    v->pitch_ramp_ticks = v->vowel_ramp_ticks = 0;
    v->random_jitter = 4.0f;
    v->jitter_counter = 0;
    v->jitter_period = 4586;
    v->rng_state = 12345u + (uint32_t)i * 7919u;  /* staggered: unison voices are separate singers */
    v->vibrato_phase = (float)(SINE_SIZE * i) / MAX_UNISON;
    v->vibrato_depth = 0.0f;
    v->vibrato_rate = 0.5f;
    v->rate_scale = 1.0f;
    v->ramp_period = (int)(MK_FS * 0.01f);
    v->ramp_counter = v->ramp_period;
    v->current_vowel = v->target_vowel = 0.5f;
    v->current_voice = 0.5f;
    v->aspiration_amp = 0.5f;
    v->grain_dirty = 1;
    v->g_vowel = -1.0f;
    v->last_step = 0x7FFFFFFF;
    v->overlap_write_pos = v->overlap_read_pos = v->overlap_offset = 0;
    v->env_stage = ENV_IDLE;
    v->env_level = 0.0f;
    v->env_attack = v->env_decay = v->env_release = 0.0f;
    v->env_sustain = 1.0f;
}

static int has_env(const mk_voice_t *v)
{
    return v->env_attack > 0.0f || v->env_decay > 0.0f || v->env_sustain < 1.0f || v->env_release > 0.0f;
}

static void voice_note_on(mk_voice_t *v, float note)
{
    uint8_t was = v->active;
    v->active = 1;
    v->target_pitch = note;
    if (!was) {
        v->current_pitch = note;
        v->current_vowel = v->target_vowel;   /* start on the vowel, not where the last note left it */
        v->vowel_ramp_ticks = 0;
        v->grain_dirty = 1;
        v->overlap_write_pos = v->overlap_read_pos;   /* the buffer drains by itself: no pop */
        v->overlap_offset = 0;
    }
    if (has_env(v)) {
        v->env_stage = ENV_ATTACK;            /* from where it is: no click on a retrigger */
    } else {
        v->env_level = 1.0f;
        v->env_stage = ENV_SUSTAIN;
    }
}

static void voice_note_off(mk_voice_t *v)
{
    if (has_env(v)) {
        v->env_stage = ENV_RELEASE;
    } else {
        v->active = 0;
        v->env_stage = ENV_IDLE;
        v->env_level = 0.0f;
    }
}

static int voice_is_active(const mk_voice_t *v) { return v->active || v->env_stage == ENV_RELEASE; }

static void voice_set_pitch_direct(mk_voice_t *v, float note) { v->target_pitch = v->current_pitch = note; }

static void voice_set_vowel(mk_voice_t *v, float vowel)
{
    float t = clamp01(vowel), eff = v->glide_param > v->min_glide ? v->glide_param : v->min_glide;
    if (eff < 0.001f) {
        v->current_vowel = t;
        v->vowel_ramp_ticks = 0;
    } else {
        int n = RAMP_TICKS;
        if (eff > 0.5f)
            n = RAMP_TICKS + (int)((eff - 0.5f) * 180.0f);
        v->vowel_step = (t - v->current_vowel) / (float)n;
        v->vowel_ramp_ticks = n;
    }
    v->target_vowel = t;
}

static float voice_amplitude(const mk_voice_t *v)   /* upstream's pitch compensation */
{
    return clampf((v->current_pitch - 12.0f) * (-1.0f / 72.0f) + 2.0f, 0.1f, 3.0f);
}

static void voice_process(mk_voice_t *v, float *out, uint32_t n)
{
    int env = has_env(v) || v->env_stage == ENV_RELEASE;
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (v->active || v->env_stage == ENV_RELEASE) {
            float pitch;
            uint32_t period;
            apply_portamento(v);
            pitch = v->current_pitch + v->pitch_bend_offset + compute_vibrato(v);
            period = grain_period(v, pitch);
            if (v->overlap_offset >= period || v->grain_dirty) {
                if (v->grain_dirty || v->current_vowel != v->g_vowel || v->current_voice != v->g_voice ||
                    v->aspiration_amp != v->g_asp)
                    compute_grain(v);
                overlap_add(v);
                v->overlap_offset = 0;
            }
        }
        v->overlap_offset++;
    }
    for (i = 0; i < n; i++) {
        uint32_t r = v->overlap_read_pos & (OLAP - 1u);
        float s = v->overlap_buf[r];
        v->overlap_buf[r] = 0.0f;
        v->overlap_read_pos = r + 1u;
        if (env)
            s *= env_tick(v);
        out[i] += s;
    }
}

/* ------------------------------------------------------------------- delay */
static void delay_taps(void)
{
    float rs = 0.1f + D.rate * 1.9f;               /* the taps (~310 / ~398 ms at rate 0.5), half rate */
    D.target_l = fm_minf(13653.0f * rs * 0.5f, (float)(DL_N - 2u));
    D.target_r = fm_minf(17570.0f * rs * 0.5f, (float)(DL_N - 2u));
}

static void delay_init(void)
{
    uint32_t i;
    for (i = 0; i < DL_N; i++)
        D.l[i] = D.r[i] = 0;
    D.w = 0;
    D.coeff = 2.0f / (0.05f * MK_FS);             /* ~50 ms smoothing, per half-rate step */
    D.rate = 0.5f;
    D.feedback = 0.5f;
    D.mix = 0.5f;
    delay_taps();
    D.cur_l = D.target_l;
    D.cur_r = D.target_r;
    D.tap_l = D.tap_r = 0.0f;
}

static inline float dl_read(const int16_t *b, float pos)
{
    float rd = (float)D.w - pos, fr;
    int i;
    if (rd < 0.0f)
        rd += (float)DL_N;
    i = (int)rd;
    fr = rd - (float)i;
    return ((float)b[i] * (1.0f - fr) + (float)b[(uint32_t)(i + 1) % DL_N] * fr) * (1.0f / DL_SCALE);
}

static inline int16_t dl_s16(float x)
{
    x *= DL_SCALE;
    return (int16_t)(x > 32767.0f ? 32767.0f : x < -32767.0f ? -32767.0f : x);
}

/* dry + the taps; each channel feeds back into itself. Two input samples make one half-rate step; the
 * taps are upsampled by a linear step between the last two */
static void delay_process(const float *in, float *ol, float *or_, uint32_t n)
{
    uint32_t i;
    for (i = 0; i + 1u < n; i += 2u) {
        float x = 0.5f * (in[i] + in[i + 1u]), tl, tr;
        D.cur_l += D.coeff * (D.target_l - D.cur_l);
        D.cur_r += D.coeff * (D.target_r - D.cur_r);
        tl = dl_read(D.l, D.cur_l);
        tr = dl_read(D.r, D.cur_r);
        D.l[D.w] = dl_s16((tl * D.feedback + x) * D.mix);
        D.r[D.w] = dl_s16((tr * D.feedback + x) * D.mix);
        D.w = (D.w + 1u) % DL_N;
        ol[i] = in[i] + 0.5f * (D.tap_l + tl);
        or_[i] = in[i] + 0.5f * (D.tap_r + tr);
        ol[i + 1u] = in[i + 1u] + tl;
        or_[i + 1u] = in[i + 1u] + tr;
        D.tap_l = tl;
        D.tap_r = tr;
    }
}

/* ------------------------------------------------------------------- synth */
static float detuned(float base, int i, int n)
{
    if (n <= 1)
        return base;
    return base + S.unison_detune * ((float)i / (float)(n - 1) * 2.0f - 1.0f) * 0.01f;   /* cents */
}

static void apply_voice_spread(void)
{
    int n = S.unison_count, i;
    for (i = 0; i < n; i++) {
        float off = n > 1 ? S.unison_voice_spread * ((float)i / (float)(n - 1) * 2.0f - 1.0f) : 0.0f;
        S.v[i].current_voice = clamp01(S.base_voice + off);
    }
}

static void apply_unison(float base)
{
    int n = S.unison_count, i;
    S.last_base_note = base;
    for (i = 0; i < n; i++)
        voice_note_on(&S.v[i], detuned(base, i, n));
}

static void remove_note(uint8_t note)
{
    uint32_t i, j = 0;
    for (i = 0; i < S.held_count; i++)
        if (S.held[i] != note)
            S.held[j++] = S.held[i];
    S.held_count = (uint8_t)j;
}

static void synth_note_on(uint8_t note)
{
    int i;
    for (i = 0; i < MAX_UNISON; i++)
        S.v[i].min_glide = 0.0f;
    remove_note(note);
    if (S.held_count < MAX_NOTES)
        S.held[S.held_count++] = note;
    apply_unison((float)note);
}

static void synth_note_off(uint8_t note)
{
    int i;
    remove_note(note);
    if (S.held_count) {
        apply_unison((float)S.held[S.held_count - 1u]);
    } else {
        for (i = 0; i < MAX_UNISON; i++)
            voice_note_off(&S.v[i]);
    }
}

static void synth_set_unison(int count)
{
    int old = S.unison_count, i;
    count = count < 1 ? 1 : count > MAX_UNISON ? MAX_UNISON : count;
    S.unison_count = count;
    S.target_gain = fm_rsqrtf((float)count);
    if (old == count || !voice_is_active(&S.v[0]))
        return;
    for (i = count; i < old; i++)                  /* fade the ones let go (the 3 ms anti-click) */
        S.v[i].env_stage = ENV_RELEASE;
    for (i = 0; i < count && i < old; i++)
        voice_set_pitch_direct(&S.v[i], detuned(S.last_base_note, i, count));
    for (i = old; i < count; i++)
        voice_note_on(&S.v[i], detuned(S.last_base_note, i, count));
    apply_voice_spread();
}

static void synth_set_detune(float cents)
{
    int n = S.unison_count, i;
    S.unison_detune = cents;
    if (voice_is_active(&S.v[0]) && n > 1)
        for (i = 0; i < n; i++)
            voice_set_pitch_direct(&S.v[i], detuned(S.last_base_note, i, n));
}

#define ALLV(stmt)                                                                                      \
    do {                                                                                                \
        int _i;                                                                                         \
        for (_i = 0; _i < MAX_UNISON; _i++) {                                                           \
            mk_voice_t *v = &S.v[_i];                                                                   \
            stmt;                                                                                       \
        }                                                                                               \
    } while (0)

/* a parameter into the engine, with the iOS port's curves (RenderContext.swift) */
static void engine_param(int id, float x)
{
    switch (id) {
    case MK_GLIDE: ALLV(v->glide_param = x); break;
    case MK_DELAY: D.mix = clamp01(x); break;
    case MK_HEAD: S.base_voice = x; apply_voice_spread(); break;
    case MK_VIBRATO: ALLV(v->vibrato_depth = clamp01(x)); break;
    case MK_VIBRATE: ALLV(v->vibrato_rate = clamp01(x); v->rate_scale = fm_exp2f((v->vibrato_rate * 2.0f - 1.0f) * 2.0f)); break;
    case MK_BREATH: ALLV(v->aspiration_amp = clamp01(x)); break;
    case MK_ATTACK: ALLV(v->env_attack = x * 5.0f); break;
    case MK_DECAY: ALLV(v->env_decay = x * 5.0f); break;
    case MK_SUSTAIN: ALLV(v->env_sustain = clamp01(x)); break;
    case MK_RELEASE: ALLV(v->env_release = x * 5.0f); break;
    case MK_VOICES: synth_set_unison((int)(x * 9.0f + 1.5f)); break;
    case MK_DETUNE: synth_set_detune(x * 50.0f); break;
    case MK_DELAYRATE: D.rate = clamp01(x); delay_taps(); break;
    case MK_LEVEL: S.level = x; break;
    case MK_SPREAD: S.unison_voice_spread = x * 0.5f; apply_voice_spread(); break;
    default: break;
    }
}

/* ------------------------------------------------- the wrapper: queue, slew, pressure, velocity */
enum { C_ON, C_OFF, C_PARAM, C_PRESS, C_TOUCH, C_PANIC };
typedef struct {
    uint8_t type, a, b;
    float f, g;
} mk_cmd_t;
#define NCMD 64u
static mk_cmd_t cmdq[NCMD];
static volatile uint32_t cmd_w, cmd_r;

static struct {
    float p[MK_NPARAM], cur[MK_NPARAM];       /* the knobs' targets and the slewed values */
    float expr, expr_cur;                     /* pressure */
    int route;
    float depth, vel_sens;
    float vel_gain, vel_target;
    float bend_cur;
    uint8_t notes_held;
} W;

#define SLEW_COEF 0.15f                       /* per 256-sample block: ~35 ms */
#define SLEW_SNAP 0.002f
#define BEND_RANGE 2.0f
static const uint8_t SMOOTH[MK_NPARAM] = {     /* knobs slewed rather than stepped (Schwung's choice) */
    [MK_VOWEL] = 1, [MK_DELAY] = 1, [MK_HEAD] = 1, [MK_VIBRATO] = 1, [MK_VIBRATE] = 1, [MK_BREATH] = 1,
    [MK_DETUNE] = 1, [MK_LEVEL] = 1, [MK_SPREAD] = 1, [MK_TUNE] = 1,
};

static void push(uint8_t type, uint8_t a, uint8_t b, float f, float g)
{
    uint32_t w = cmd_w, nx = (w + 1u) % NCMD;
    if (nx == cmd_r)
        return;                               /* full: dropped (64 deep; the UI sends a few a frame) */
    cmdq[w].type = type;
    cmdq[w].a = a;
    cmdq[w].b = b;
    cmdq[w].f = f;
    cmdq[w].g = g;
    __asm__ volatile("" ::: "memory");
    cmd_w = nx;
}

void monk_note_on(int note, int vel)
{
    if (note >= 0 && note < 128)
        push(C_ON, (uint8_t)note, (uint8_t)(vel < 1 ? 1 : vel > 127 ? 127 : vel), 0.0f, 0.0f);
}
void monk_note_off(int note)
{
    if (note >= 0 && note < 128)
        push(C_OFF, (uint8_t)note, 0, 0.0f, 0.0f);
}
void monk_param(int id, float v)
{
    if (id >= 0 && id < MK_NPARAM)
        push(C_PARAM, (uint8_t)id, 0, clamp01(v), 0.0f);
}
void monk_pressure(float p) { push(C_PRESS, 0, 0, clamp01(p), 0.0f); }
void monk_touch(int route, float depth, float vel_sens)
{
    push(C_TOUCH, (uint8_t)(route < 0 || route >= MK_NAT ? 0 : route), 0, clamp01(depth), clamp01(vel_sens));
}
void monk_panic(void) { push(C_PANIC, 0, 0, 0.0f, 0.0f); }

static float vel_gain_of(int vel)
{
    float x = (float)vel * (1.0f / 127.0f);
    return (1.0f - W.vel_sens) + W.vel_sens * x * x * 1.6f;   /* 1.6: a medium press (~100) keeps the level */
}

static float combined_vowel(void)
{
    float d = W.expr_cur * W.depth, v = W.cur[MK_VOWEL];
    switch (W.route) {
    case MK_AT_BOTH_INV: return clamp01(v - d);
    case MK_AT_PITCH: return v;
    default: return clamp01(v + d);
    }
}

static void apply_bend(void)
{
    float b = (W.route == MK_AT_VOWEL ? 0.0f : W.expr_cur * BEND_RANGE) + (W.cur[MK_TUNE] - 0.5f) * 24.0f;
    if (b != W.bend_cur) {
        W.bend_cur = b;
        ALLV(v->pitch_bend_offset = clampf(b, -12.0f, 12.0f));
    }
}

static void drain(void)
{
    while (cmd_r != cmd_w) {
        mk_cmd_t c = cmdq[cmd_r];
        cmd_r = (cmd_r + 1u) % NCMD;
        switch (c.type) {
        case C_ON:
            W.vel_target = vel_gain_of(c.b);
            if (!S.held_count)
                W.vel_gain = W.vel_target;         /* a new phrase starts at its own level */
            synth_note_on(c.a);
            break;
        case C_OFF:
            synth_note_off(c.a);
            break;
        case C_PARAM:
            W.p[c.a] = c.f;
            if (!SMOOTH[c.a]) {
                W.cur[c.a] = c.f;
                engine_param(c.a, c.f);
            }
            break;
        case C_PRESS:
            W.expr = c.f;
            break;
        case C_TOUCH:
            W.route = c.a;
            W.depth = c.f;
            W.vel_sens = c.g;
            break;
        case C_PANIC:
            S.held_count = 0;
            ALLV(v->active = 0; v->env_stage = ENV_IDLE; v->env_level = 0.0f);
            W.expr = W.expr_cur = 0.0f;
            break;
        default:
            break;
        }
    }
}

static void slew_block(void)
{
    int i;
    float de = W.expr - W.expr_cur;
    for (i = 0; i < MK_NPARAM; i++) {
        float d;
        if (!SMOOTH[i])
            continue;
        d = W.p[i] - W.cur[i];
        if (d == 0.0f)
            continue;
        W.cur[i] = (d > -SLEW_SNAP && d < SLEW_SNAP) ? W.p[i] : W.cur[i] + d * SLEW_COEF;
        if (i != MK_VOWEL && i != MK_TUNE)
            engine_param(i, W.cur[i]);
    }
    W.expr_cur = (de > -SLEW_SNAP && de < SLEW_SNAP) ? W.expr : W.expr_cur + de * SLEW_COEF;
    {
        float vw = combined_vowel();
        if (vw != S.v[0].target_vowel)
            ALLV(voice_set_vowel(v, vw));
    }
    apply_bend();
    W.vel_gain += (W.vel_target - W.vel_gain) * 0.2f;
}

void monk_init(void)
{
    static const float DEF[MK_NPARAM] = {   /* upstream's defaults (processor.h) */
        0.5f, 0.5f, 0.8f, 0.5f, 0.0f, 0.5f, 0.5f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.5f, 1.0f, 0.0f, 0.5f};
    int i;
    build_tables();
    for (i = 0; i < MAX_UNISON; i++)
        voice_init(&S.v[i], i);
    S.unison_count = 1;
    S.unison_detune = 0.0f;
    S.cur_gain = S.target_gain = 1.0f;
    S.held_count = 0;
    S.cc_volume = 0.1f;
    S.level = 1.0f;
    S.base_voice = 0.5f;
    delay_init();
    cmd_w = cmd_r = 0;
    W.route = MK_AT_VOWEL;
    W.depth = 0.5f;
    W.vel_sens = 0.5f;
    W.vel_gain = W.vel_target = 1.0f;
    W.expr = W.expr_cur = 0.0f;
    W.bend_cur = 1e9f;
    for (i = 0; i < MK_NPARAM; i++) {
        W.p[i] = W.cur[i] = DEF[i];
        engine_param(i, DEF[i]);
    }
    ALLV(voice_set_vowel(v, 0.5f));
    apply_bend();
}

static float mono[BLOCK], wl[BLOCK], wr[BLOCK];

void monk_render(int32_t *out, uint32_t frames, uint32_t master_q12)
{
    float g = (float)master_q12 * (1.0f / 4096.0f) * 8300000.0f, gain, gstep;
    uint32_t i, n = frames > BLOCK ? BLOCK : frames;
    int k;
    drain();
    slew_block();
    for (i = 0; i < n; i++)
        mono[i] = 0.0f;
    for (k = 0; k < MAX_UNISON; k++) {
        if (k >= S.unison_count && !voice_is_active(&S.v[k]))
            continue;
        voice_process(&S.v[k], mono, n);
    }
    gstep = (S.target_gain - S.cur_gain) / (float)n;
    for (i = 0; i < n; i++) {
        mono[i] *= S.cur_gain;
        S.cur_gain += gstep;
    }
    S.cur_gain = S.target_gain;
    delay_process(mono, wl, wr, n);
    gain = voice_amplitude(&S.v[0]) * S.cc_volume * S.level * W.vel_gain;
    for (i = 0; i < n; i++) {
        float l = fm_clampf(fm_tanhf(wl[i] * gain), -0.999f, 0.999f);
        float r = fm_clampf(fm_tanhf(wr[i] * gain), -0.999f, 0.999f);
        out[2u * i] = (int32_t)(l * g);
        out[2u * i + 1u] = (int32_t)(r * g);
    }
    monk_ui_vowel = S.v[0].current_vowel;
    monk_ui_amp = clamp01(voice_amplitude(&S.v[0]));
    monk_ui_active = S.held_count != 0;
}
