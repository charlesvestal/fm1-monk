/* SPDX-License-Identifier: GPL-3.0-only */
/* MONK-1's app on the computer: the real ui.c, face.c, project.c, sense.c and the voice, over a
 * framebuffer and a scripted panel. Runs a short session (a character, a note sung with pressure, the
 * knobs, the pages, HOLD, the next singer), checks what it can (the notes reach the voice, the
 * settings change, the screen draws), and writes screenshots (raw RGB565, big-endian) for
 * tests/ui_shots.py. Also the audio: each render's peak, so a session that goes silent or clips fails. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define OM_HOST 1

/* ---- the panel and the screen, simulated */
static uint16_t fb[240 * 240];
static uint32_t now_ms;
static void lcd_sync(void) {}
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    uint32_t r, c;
    for (r = 0; r < h; r++)
        for (c = 0; c < w; c++)
            if (x + c < 240u && y + r < 240u)
                fb[(y + r) * 240u + x + c] = px[r * w + c];
}
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    uint32_t r, k;
    for (r = 0; r < h; r++)
        for (k = 0; k < w; k++)
            if (x + k < 240u && y + r < 240u)
                fb[(y + r) * 240u + x + k] = (uint16_t)((c >> 8) | (c << 8));
}
#include "gfx.c"
static inline int32_t clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
#include "app/plat.h"

static uint32_t sim_btn, sim_keys;
static int32_t sim_enc[NE];
static uint32_t sim_leds_b;
static uint8_t store[OBJ_NOBJ][PLAT_STORE_MAX];
static int store_len[OBJ_NOBJ];
static int notes_out, at_out;
uint32_t plat_ms(void) { return now_ms; }
uint32_t plat_buttons(void) { return sim_btn; }
uint32_t plat_keys(void) { return sim_keys; }
int32_t plat_enc(int role) { int32_t e = sim_enc[role]; sim_enc[role] = 0; return e; }
uint32_t plat_master(void) { return 4096; }
void plat_leds(uint32_t buttons, uint32_t keys) { sim_leds_b = buttons; (void)keys; }
void plat_analog_config(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e) { (void)a; (void)b; (void)c; (void)d; (void)e; }
void plat_analog_selftest(uint32_t on) { (void)on; }
uint32_t plat_analog_mask(void) { return 0xFFFFFu; }   /* keys 0..19 on the analog rows, as on the FM-1 */
uint32_t plat_analog_us(void) { return 60; }
uint32_t plat_analog_timeouts(void) { return 0; }
uint32_t plat_cap_probe(uint32_t row, uint32_t n) { (void)row; (void)n; return 0; }
void plat_midi3(uint32_t st, uint32_t d1, uint32_t d2) { (void)d1; (void)d2; if ((st & 0xF0u) == 0x90u) notes_out++; if ((st & 0xF0u) == 0xA0u) at_out++; }
void plat_sysex(const uint8_t *d, uint32_t n) { (void)d; (void)n; }
uint32_t plat_midi_dropped(void) { return 0; }
int plat_midi_in(uint32_t *pkt) { (void)pkt; return 0; }
void plat_midi_out(uint32_t pkt) { (void)pkt; }
int plat_store_load(uint32_t obj, void *dst, uint32_t max)
{
    if (!store_len[obj])
        return -1;
    memcpy(dst, store[obj], (size_t)(store_len[obj] < (int)max ? store_len[obj] : (int)max));
    return store_len[obj];
}
int plat_store_save(uint32_t obj, const void *src, uint32_t len)
{
    memcpy(store[obj], src, len);
    store_len[obj] = (int)len;
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

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static float peak;
static void step(uint32_t ms)              /* time passes: the scan, the audio, the UI */
{
    static int32_t out[512];
    uint32_t t;
    for (t = 0; t < ms; t++) {
        now_ms++;
        if (now_ms % 6u == 0u) {           /* ~ one 256-frame render every 5.8 ms */
            uint32_t i;
            monk_render(out, 256, 4096);
            for (i = 0; i < 512u; i++) {
                float v = (float)abs(out[i]) / 8300000.0f;
                if (v > peak)
                    peak = v;
            }
        }
        if (now_ms % 16u == 0u)
            ui_frame();
        else
            ui_input_only();
    }
}

/* a press of analog key k: the sensing's samples, the pulled-up reading falling to `land`, the
 * driven one at `dland` (lower = firmer) */
static void press(uint32_t k, uint16_t land, uint16_t dland)
{
    uint32_t i;
    for (i = 0; i < 6u; i++) {
        sense_sample(k, i == 0 ? 1000u : land, now_ms * 24000u, 0);
        sense_sample(k, dland, now_ms * 24000u, 1);
        step(2);
    }
}
static void hold_press(uint32_t k, uint16_t land, uint16_t dland, uint32_t ms)
{
    uint32_t t;
    for (t = 0; t < ms; t += 2u) {
        sense_sample(k, land, now_ms * 24000u, 0);
        sense_sample(k, dland, now_ms * 24000u, 1);
        step(2);
    }
}
static void release(uint32_t k)
{
    uint32_t i;
    for (i = 0; i < 6u; i++) {
        sense_sample(k, 1023u, now_ms * 24000u, 0);
        step(2);
    }
}
static void tap(int b)
{
    sim_btn |= 1u << b;
    step(40);
    sim_btn &= ~(1u << b);
    step(40);
}

static void shot(const char *dir, const char *name)
{
    char path[256];
    FILE *f;
    snprintf(path, sizeof path, "%s/%s.565", dir, name);
    f = fopen(path, "wb");
    if (f) {
        fwrite(fb, 2, 240 * 240, f);
        fclose(f);
    }
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    float v0;
    sense_init(plat_analog_mask());
    sense_setup();
    monk_init();
    project_load();
    project_apply();
    ui_init();
    step(300);
    shot(dir, "01_boot");
    CHECK(ui.scene == 0, "the first character is on screen");

    step(2500);                        /* the tag times out */
    shot(dir, "02_monk");
    peak = 0.0f;
    press(5, 300, 560);                /* a firm press */
    hold_press(5, 300, 560, 300);
    CHECK(notes_out == 1, "the note went out (%d)", notes_out);
    CHECK(monk_ui_active, "the voice sings");
    CHECK(peak > 0.05f && peak < 1.0f, "a sung note is heard (peak %.3f)", (double)peak);
    shot(dir, "03_singing");
    hold_press(5, 300, 500, 300);      /* leaning on it: pressure */
    CHECK(at_out > 0, "aftertouch went out");
    CHECK(monk_ui_vowel > 0.55f, "pressure opened the vowel (%.2f)", (double)monk_ui_vowel);
    shot(dir, "04_pressure");
    release(5);
    step(300);
    CHECK(!monk_ui_active, "let go");

    v0 = cur_p()[MK_HEAD];
    sim_enc[EN_K2] = 5;                /* HEAD on SING */
    step(100);
    CHECK(cur_p()[MK_HEAD] > v0, "K2 turned HEAD");
    shot(dir, "05_knob");
    tap(B_ENV);
    CHECK(proj.page == PG_ENV, "ENV: the envelope page");
    shot(dir, "06_env");
    tap(B_GLO);
    CHECK(proj.page == PG_TOUCH, "GLO: the touch page");
    tap(B_FX);
    CHECK(proj.page == PG_ECHO, "FX: the echo page");
    shot(dir, "06b_echo");
    tap(B_GLO);
    sim_enc[EN_K1] = 1;
    step(100);
    CHECK(proj.at_route == MK_AT_PITCH, "PRESSURE: pitch");
    shot(dir, "07_touch");
    tap(B_PLAY);
    CHECK(ui.hold, "PLAY: HOLD");
    press(9, 300, 600);
    release(9);
    step(200);
    CHECK(monk_ui_active, "HOLD keeps the note");
    tap(B_PLAY);
    step(200);
    CHECK(!monk_ui_active, "HOLD off lets it go");

    sim_enc[EN_PRESET] = 1;            /* the next singer */
    step(400);
    CHECK(proj.character == 1 && ui.scene == 1, "PRESETS: the fish");
    shot(dir, "08_fish");
    sim_enc[EN_PRESET] = 1;
    step(3000);
    CHECK(proj.character == 2, "the unicorn");
    press(12, 300, 600);
    hold_press(12, 300, 600, 500);
    shot(dir, "09_unicorn_singing");
    release(12);
    step(2000);
    {
        int c;
        for (c = 3; c < ART_N; c++) {
            char n[32];
            sim_enc[EN_PRESET] = 1;
            step(2600);
            snprintf(n, sizeof n, "1%d_%s", c, ART[c].id);
            shot(dir, n);
        }
    }
    step(6000);                        /* the idle animation: blinks and shuffles; the autosave */
    CHECK(store_len[OBJ_PROJ] > 0, "the settings were saved");
    CHECK(sim_leds_b & (1u << B_PLAY) ? 0 : 1, "HOLD's light is off");
    printf(fails ? "ui: %d failures\n" : "ui: ok\n", fails);
    return fails != 0;
}
