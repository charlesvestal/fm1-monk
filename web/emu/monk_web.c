/* SPDX-License-Identifier: GPL-3.0-only */
/* MONK-1 in the browser: the real ui.c, face.c, project.c, sense.c and the voice (as tests/host/ui_sim.c
 * runs them) over a framebuffer and a panel the page drives, compiled to WebAssembly and run in an
 * AudioWorklet (from FoMni's). The worklet asks for audio; producing it advances the device's clock one
 * millisecond at a time, which renders the voice every 256 frames and the UI every 16 ms, as on the FM-1.
 *
 * The keys: the page says, for each key held, how hard it landed (a velocity) and how hard it is
 * pressed now (0..1). The analog keys (0..19, F3 to C5) turn that into the readings Velo's
 * sensing would get from the FM-1 (a pulled-up sample under PRESS, a driven one at the depth that the
 * key's calibration maps to that velocity, then deeper by the lean that ui.c's pressure() reads as that
 * pressure), so the velocity and the pressure go through the firmware's own code. The seven highest
 * keys (the matrix's PA6 row) have no converter on the FM-1: their velocity comes from the PA10 probe's
 * rise, which these give it (sense_probe); they have no pressure.
 *
 *   web/emu/build.sh   ->  build/emu/monk.wasm (+ the page) */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define OM_HOST 1
#define OM_WEB 1

/* ---- the screen */
static uint16_t fb[240 * 240];
static uint32_t now_ms, blits;
static void lcd_sync(void) {}
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    uint32_t r, c;
    for (r = 0; r < h; r++)
        for (c = 0; c < w; c++)
            if (x + c < 240u && y + r < 240u)
                fb[(y + r) * 240u + x + c] = px[r * w + c];
    blits++;
}
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    uint32_t r, k;
    for (r = 0; r < h; r++)
        for (k = 0; k < w; k++)
            if (x + k < 240u && y + r < 240u)
                fb[(y + r) * 240u + x + k] = (uint16_t)((c >> 8) | (c << 8));
    blits++;
}
#include "gfx.c"
static inline int32_t clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
#include "app/plat.h"

/* ---- the panel */
static uint32_t held_btn, held_keys, master = 2800, lit_btn, lit_keys;
static int32_t enc_acc[NE];
static uint8_t store[OBJ_NOBJ][PLAT_STORE_MAX];
static int store_len[OBJ_NOBJ] = {-1, -1};
static uint32_t store_writes;
#define ANALOG 0xFFFFFu                /* keys 0..19 on the analog rows, as on the FM-1 */
uint32_t plat_ms(void) { return now_ms; }
uint32_t plat_buttons(void) { return held_btn; }
uint32_t plat_keys(void) { return held_keys; }
int32_t plat_enc(int role) { int32_t e = enc_acc[role]; enc_acc[role] = 0; return e; }
uint32_t plat_master(void) { return master; }
void plat_leds(uint32_t buttons, uint32_t keys) { lit_btn = buttons; lit_keys = keys; }
void plat_analog_config(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e) { (void)a; (void)b; (void)c; (void)d; (void)e; }
void plat_analog_selftest(uint32_t on) { (void)on; }
uint32_t plat_analog_mask(void) { return ANALOG; }
uint32_t plat_analog_us(void) { return 60; }
uint32_t plat_analog_timeouts(void) { return 0; }
uint32_t plat_cap_probe(uint32_t row, uint32_t n) { (void)row; (void)n; return 0; }
void plat_midi3(uint32_t st, uint32_t d1, uint32_t d2) { (void)st; (void)d1; (void)d2; }
void plat_sysex(const uint8_t *d, uint32_t n) { (void)d; (void)n; }
uint32_t plat_midi_dropped(void) { return 0; }
int plat_midi_in(uint32_t *pkt) { (void)pkt; return 0; }
void plat_midi_out(uint32_t pkt) { (void)pkt; }
int plat_store_load(uint32_t obj, void *dst, uint32_t max)
{
    if (obj >= OBJ_NOBJ || store_len[obj] <= 0)
        return -1;
    memcpy(dst, store[obj], (size_t)(store_len[obj] < (int)max ? store_len[obj] : (int)max));
    return store_len[obj];
}
int plat_store_save(uint32_t obj, const void *src, uint32_t len)
{
    if (obj >= OBJ_NOBJ || len > PLAT_STORE_MAX)
        return -1;
    memcpy(store[obj], src, len);
    store_len[obj] = (int)len;
    store_writes++;
    return 0;
}
uint32_t plat_cpu_pct(void) { return 0; }
uint32_t plat_xruns(void) { return 0; }

#include "app/app.h"
#include "app/sense.c"
#include "app/project.c"
#include "app/inflate.c"
#include "app/face.c"
#include "app/ui.c"

#undef __attribute__
#define EXPORT __attribute__((used, visibility("default")))

/* ---- the keys, as the sensing reads them */
#define PULL_DOWN 300u                 /* a seated key's pulled-up reading (under PRESS) */
static uint8_t k_vel[NKEYS];           /* 0 = up; else the velocity it lands with */
static float k_press[NKEYS];           /* 0..1 */
static uint16_t k_land[NKEYS];         /* the driven reading it landed at */
static uint32_t k_t0[NKEYS];           /* when it seated */

/* the driven reading that key k's calibration maps to velocity v (sense_velocity_d, inverted) */
static uint16_t land_for(uint32_t k, uint32_t v)
{
    const sense_key_t *s = &sn_key[k];
    float r0 = (float)(s->dfloor > SN_DBASE ? s->dfloor - SN_DBASE : 1u), r1 = (float)(s->dceil - SN_DBASE);
    float c = (float)(sn_cfg.d_curve ? sn_cfg.d_curve : 1u) * 0.01f, t;
    if (v >= 127u)
        return (uint16_t)(s->dfloor > 1u ? s->dfloor - 1u : 1u);
    t = 1.0f - powf(((float)v - 1.0f) / 126.0f, 1.0f / c);
    return (uint16_t)(SN_DBASE + (uint32_t)(r0 * powf(r1 / r0, t) + 0.5f));
}

static void scan(void)                 /* every 2 ms: each analog key's samples */
{
    uint32_t k;
    for (k = 0; k < NKEYS; k++) {
        if (!(ANALOG >> k & 1u))
            continue;
        if (!k_vel[k]) {
            if (sn_key[k].st != SN_OPEN)
                sense_sample(k, 1023u, now_ms * 24000u, 0);
            continue;
        }
        if (sn_key[k].st == SN_OPEN) {
            k_land[k] = land_for(k, k_vel[k]);
            k_t0[k] = 0;
        }
        sense_sample(k, PULL_DOWN, now_ms * 24000u, 0);
        if (sense_want_drive(k)) {
            uint32_t d = k_land[k];
            if (sn_key[k].st == SN_DOWN && !k_t0[k])
                k_t0[k] = now_ms;
            /* past ui.c's settling (its zero follows the key down for P_SETTLE ms): the lean that
             * pressure() reads as k_press, below where it has the key resting */
            if (k_t0[k] && now_ms - k_t0[k] > P_SETTLE + 20u && p_on[k] && k_press[k] > 0.0f) {
                float lean = P_DEAD + k_press[k] * P_SPAN((float)proj.p_sens * 0.01f);
                float r = p_base[k] / powf(2.0f, lean);
                d = SN_DBASE + (uint32_t)(r > 2.0f ? r : 2.0f);
            }
            sense_sample(k, d, now_ms * 24000u, 1);
        }
    }
}

static uint32_t ui_due;
static double audio_due;
static int32_t blk[512];
#define RING 4096u
static float ring[RING * 2];
static uint32_t ring_w, ring_r;

static void run_ms(uint32_t ms)
{
    uint32_t i, k;
    for (i = 0; i < ms; i++) {
        now_ms++;
        if (!(now_ms & 1u))
            scan();
        while (audio_due <= (double)now_ms) {      /* 256-frame blocks, as the device renders them */
            monk_render(blk, 256, (master * master) >> 12);   /* MASTER's squared curve, as main_fm1.c */
            for (k = 0; k < 256u; k++) {
                uint32_t j = (ring_w++ % RING) * 2u;
                ring[j] = (float)blk[2 * k] * (1.0f / 8388608.0f);
                ring[j + 1] = (float)blk[2 * k + 1] * (1.0f / 8388608.0f);
            }
            audio_due += 256.0 * 1000.0 / 44100.0;
        }
        if (now_ms >= ui_due) {
            ui_due = now_ms + 16u;
            ui_frame();
        } else {
            ui_input_only();
        }
    }
}

static float out_l[1024], out_r[1024];
EXPORT float *web_out_l(void) { return out_l; }
EXPORT float *web_out_r(void) { return out_r; }

/* n frames (<= 1024) into out_l / out_r: the device runs until they exist */
EXPORT void web_render(uint32_t n)
{
    uint32_t k;
    if (n > 1024u)
        n = 1024u;
    while (ring_w - ring_r < n)
        run_ms(1);
    for (k = 0; k < n; k++) {
        uint32_t j = (ring_r++ % RING) * 2u;
        out_l[k] = ring[j];
        out_r[k] = ring[j + 1];
    }
}

EXPORT void web_boot(void)
{
    sense_init(plat_analog_mask());
    sense_setup();
    monk_init();
    project_load();
    project_apply();
    ui_init();
}
EXPORT void web_buttons(uint32_t m) { held_btn = m; }
/* key k: vel 0 = up, else held, landed at vel (1..127); press 0..1000 = how hard it is pressed now */
EXPORT void web_key(uint32_t k, uint32_t vel, uint32_t press)
{
    if (k >= NKEYS)
        return;
    k_vel[k] = (uint8_t)(vel > 127u ? 127u : vel);
    k_press[k] = (float)(press > 1000u ? 1000u : press) * 0.001f;
    if (!(ANALOG >> k & 1u)) {
        if (vel) {                     /* the probe's rise above its open reading that maps to vel */
            uint32_t lo = sn_cfg.t_lo, hi = sn_cfg.t_hi > lo ? sn_cfg.t_hi : lo + 1u, i;
            for (i = 0; i < 8u; i++)
                sense_probe(k, 100u, 0);
            sense_probe(k, 100u + lo + ((uint32_t)k_vel[k] - 1u) * (hi - lo) / 126u, 1);
            held_keys |= 1u << k;
        } else {
            sense_probe(k, 100u, 0);
            held_keys &= ~(1u << k);
        }
    }
}
EXPORT void web_enc(int role, int32_t n)
{
    if (role >= 0 && role < NE)
        enc_acc[role] += n;
}
EXPORT void web_master(uint32_t v) { master = v > 4096u ? 4096u : v; }
EXPORT uint32_t web_lit_buttons(void) { return lit_btn; }
EXPORT uint32_t web_lit_keys(void) { return lit_keys; }
EXPORT uint16_t *web_fb(void) { return fb; }
EXPORT uint32_t web_blits(void) { return blits; }
EXPORT uint32_t web_now(void) { return now_ms; }
/* what the voice is doing, for the page: the sounding note's velocity and pressure */
EXPORT uint32_t web_key_vel(uint32_t k) { return k < NKEYS ? sn_key[k].vel : 0; }
EXPORT uint32_t web_key_at(uint32_t k) { return k < NKEYS ? at_sent[k] : 0; }
/* the sounding key (the newest held): 1 << 16 | its pressure << 8 | its velocity; 0 = none */
EXPORT uint32_t web_touch(void)
{
    uint32_t k;
    if (!ui.nstack)
        return 0;
    k = ui.stack[ui.nstack - 1u];
    return 1u << 16 | (uint32_t)at_sent[k] << 8 | (sense_is_analog(k) ? sn_key[k].vel : top_vel(k));
}

/* saved objects: the page keeps them (localStorage) and gives them back before boot */
EXPORT uint8_t *web_store(uint32_t obj) { return obj < OBJ_NOBJ ? store[obj] : 0; }
EXPORT int web_store_len(uint32_t obj) { return obj < OBJ_NOBJ ? store_len[obj] : -1; }
EXPORT void web_store_set_len(uint32_t obj, int n)
{
    if (obj < OBJ_NOBJ)
        store_len[obj] = n < 0 ? -1 : n > (int)PLAT_STORE_MAX ? (int)PLAT_STORE_MAX : n;
}
EXPORT uint32_t web_store_writes(void) { return store_writes; }
EXPORT uint32_t web_nobj(void) { return OBJ_NOBJ; }
EXPORT uint32_t web_store_max(void) { return PLAT_STORE_MAX; }
