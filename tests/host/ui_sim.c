/* SPDX-License-Identifier: GPL-3.0-only */
/* MONK-1's app on the computer: the real ui.c, face.c, project.c, sense.c and the voice, over a
 * framebuffer and a scripted panel. Runs a short session (a character, a note sung with pressure, the
 * knobs, the pages, HOLD, the next singer), checks what it can (the notes reach the voice, the
 * settings change, the screen draws), and writes screenshots (raw RGB565, big-endian) for
 * tests/ui_shots.py. Also the audio: each render's peak, so a session that goes silent or clips fails.
 *
 *   ui_sim DIR                 the session above
 *   ui_sim DIR SCRIPT          a script instead (tests/scenarios/NAME.monk), one command a line (# comments):
 *     wait MS                  time passes
 *     tap BTN                  a button pressed and let go (FX SEL ENV LFO EDIT GLO HOME SAVE ARP SEQ PLAY REC OCT- OCT+)
 *     hold BTN MS              a button held MS ms, then let go
 *     turn ENC N               an encoder N detents (SELECT ALGO PRESETS K1..K4)
 *     key K [MS]               analog key K (0 = F3) pressed firmly, held MS ms (200; leaning on it the second
 *                              half: pressure), let go
 *     listen                   a screen reader starts listening (HELLO, again every 2 s): each SAY is printed
 *                              ("said: ..."; "said!:" cuts off what was being said)
 *     where                    the reader asks for everything on the screen
 *     said TEXT                some SAY since the last "said" (or "listen") has TEXT in it: else a failure
 *     silent                   nothing was said since the last "said" (or "silent"): else a failure
 *     speakdump                every page, knob value, singer and message as the reader gets them (the audit)
 *   MONK_SAID_LOG=FILE: a reader listens from boot, quietly, and every SAY is appended to FILE (the audit). */
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
#define ANN_NAME "MONK-1 " OM_VERSION
#include "ann_words.h"                                  /* the words file's link (tools/gen_words.py) */
#include "announce.c"
/* what the reader is told: every SAY to stdout with "said:" (listen), and kept for the "said" check */
static char said_last[ANN_MAX + 1], said_log[16384];   /* said_log: every SAY since the last "said" check */
static uint32_t said_len;
static int said_print = 1, sim_listen;
static FILE *said_log_f;                                /* MONK_SAID_LOG=FILE: every SAY, for fm1-reader's audit */
static int ann_wire_send(const uint8_t *p, uint32_t n)
{
    if (n >= 7u && p[4] == ANN_SAY) {
        uint32_t i, k = 0;
        for (i = 6; i + 1u < n && k < ANN_MAX; i++)
            said_last[k++] = (char)p[i];
        said_last[k] = 0;
        if (said_len + k + 2u < sizeof said_log) {
            memcpy(said_log + said_len, said_last, k);
            said_len += k;
            said_log[said_len++] = '\n';
            said_log[said_len] = 0;
        }
        if (said_print && sim_listen)
            printf("said%s: %s\n", (p[5] & ANN_NOW) ? "!" : "", said_last);
        if (said_log_f)
            fprintf(said_log_f, "%s\t%s\n", (p[5] & ANN_KEEP) ? "WARN" : "MSG", said_last);
    }
    return 0;
}
static void sim_hello(void)
{
    static const uint8_t HELLO[] = {ANN_H0, ANN_H1, ANN_H2, ANN_HELLO, ANN_VER};
    ann_frame(HELLO, sizeof HELLO, now_ms);
}
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
        if (sim_listen && now_ms % 2000u == 0u)   /* the reader's HELLO, as it sends it */
            sim_hello();
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

static int button_of(const char *n)
{
    int i;
    for (i = 0; i < NB; i++)
        if (!strcmp(n, BTN_LABEL[i]))
            return i;
    return -1;
}
static int enc_of(const char *n)
{
    static const char *const ENC[NE] = {"SELECT", "ALGO", "PRESETS", "K1", "K2", "K3", "K4"};
    int i;
    for (i = 0; i < NE; i++)
        if (!strcmp(n, ENC[i]))
            return i;
    return -1;
}

static int script(const char *path)
{
    char line[512];
    int lineno = 0;
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("FAIL: no script %s\n", path);
        return 1;
    }
    while (fgets(line, sizeof line, f)) {
        char cmd[32] = {0}, a[64] = {0}, b[64] = {0};
        int x;
        lineno++;
        if (sscanf(line, "%31s %63s %63s", cmd, a, b) < 1 || cmd[0] == '#')
            continue;
        if (!strcmp(cmd, "wait")) {
            step((uint32_t)atoi(a));
        } else if (!strcmp(cmd, "tap") || !strcmp(cmd, "hold")) {
            if ((x = button_of(a)) < 0) {
                printf("FAIL line %d: no button %s\n", lineno, a);
                fails++;
                continue;
            }
            if (cmd[0] == 't') {
                tap(x);
            } else {
                sim_btn |= 1u << x;
                step((uint32_t)atoi(b));
                sim_btn &= ~(1u << x);
                step(40);
            }
        } else if (!strcmp(cmd, "turn")) {
            if ((x = enc_of(a)) < 0) {
                printf("FAIL line %d: no encoder %s\n", lineno, a);
                fails++;
                continue;
            }
            sim_enc[x] += atoi(b);
            step(40);
        } else if (!strcmp(cmd, "key")) {
            uint32_t ms = b[0] ? (uint32_t)atoi(b) : 200u;
            x = atoi(a);
            press((uint32_t)x, 300, 560);
            hold_press((uint32_t)x, 300, 560, ms / 2u);
            hold_press((uint32_t)x, 300, 500, ms - ms / 2u);   /* leaning on it: pressure */
            release((uint32_t)x);
        } else if (!strcmp(cmd, "listen")) {
            sim_listen = 1;
            said_len = 0;
            said_log[0] = 0;
            sim_hello();
            step(40);
        } else if (!strcmp(cmd, "where")) {
            static const uint8_t WHERE[] = {ANN_H0, ANN_H1, ANN_H2, ANN_WHERE};
            ann_frame(WHERE, sizeof WHERE, now_ms);
            step(40);
        } else if (!strcmp(cmd, "speakdump")) {
            speak_dump();
        } else if (!strcmp(cmd, "silent")) {
            if (said_len) {
                printf("FAIL line %d: silent: said %s", lineno, said_log);
                fails++;
            } else if (said_print) {
                printf("  ok silent\n");
            }
            said_len = 0;
            said_log[0] = 0;
        } else if (!strcmp(cmd, "said")) {
            char *t = line + 4, *e;
            while (*t == ' ')
                t++;
            e = t + strlen(t);
            while (e > t && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '))
                *--e = 0;
            if (!strstr(said_log, t)) {
                printf("FAIL line %d: said \"%s\": not said (since the last check: %s)\n", lineno, t,
                       said_len ? said_log : "nothing\n");
                fails++;
            } else if (said_print) {
                printf("  ok said \"%s\"\n", t);
            }
            said_len = 0;
            said_log[0] = 0;
        } else {
            printf("FAIL line %d: what is \"%s\"?\n", lineno, cmd);
            fails++;
        }
    }
    fclose(f);
    return fails != 0;
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
    if (getenv("MONK_SAID_LOG")) {                 /* a reader listens from the start, quietly: the log only */
        said_log_f = fopen(getenv("MONK_SAID_LOG"), "a");
        sim_listen = 1;
        said_print = 0;
        sim_hello();
    }
    if (argc > 2) {
        int r;
        step(300);
        r = script(argv[2]);
        if (said_print)
            printf(r ? "%s: %d failures\n" : "%s: ok\n", argv[2], fails);
        return r;
    }
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

    /* two keys held when HOLD goes on, both let go, HOLD off: nothing may stay on (the first key's note
     * used to stay in the voice's note stack and sing once the latched one went) */
    press(3, 300, 600);
    press(7, 300, 600);
    tap(B_PLAY);
    release(3);
    release(7);
    step(100);
    CHECK(monk_ui_active, "HOLD keeps the latched note");
    tap(B_PLAY);
    step(200);
    CHECK(!monk_ui_active, "HOLD off with two keys: nothing stuck");
    /* the octave changed while a key is down: its release still lets its note go */
    press(4, 300, 600);
    tap(B_OCTUP);
    release(4);
    step(200);
    CHECK(!monk_ui_active, "octave change under a key: nothing stuck");
    tap(B_OCTDN);

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
