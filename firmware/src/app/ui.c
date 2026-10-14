/* SPDX-License-Identifier: GPL-3.0-only */
/* MONK-1 UI. Part of the unity build (after gfx.c, sense.c, project.c, inflate.c and face.c).
 *
 *   keys           sing (one voice: the newest key held sounds, letting go falls back to the one
 *                  under it). The velocity sets the level (VELOCITY on TOUCH); pressing harder on a
 *                  held key (the driven reading: Velo's sensing) moves the vowel, or bends, or both
 *   PRESETS        the singer: twelve characters, each with its voice, each remembering its settings
 *   ALGORITHM      the vowel (the knob everyone plays with)
 *   KNOB 1-4       the page's four values       SELECT / EDIT   the page     HOME   SING
 *   ENV            ENVELOPE                     LFO / FX        AIR (vibrato and the echo)
 *   OCT- / OCT+    octave                       PLAY            HOLD: the note stays after the key
 *   SAVE           saves now (it saves itself a few seconds after the last change)
 *   GLO twice      this character's voice back to how it came
 *
 * The screen is the character on its stage (face.c): the mouth follows the vowel, in frames, and
 * swells with loudness; idle, it blinks and shuffles through its vowels as the iOS one does. Turning
 * a knob brings up the strip of four values at the bottom; a new singer or page, its name at the top.
 * USB MIDI out: the notes (channel 1) with their velocity and polyphonic aftertouch. In: notes (any
 * channel), CC 1 vowel, 5 glide, 7 level, 12 echo, 13 head size, aftertouch, CC 123 all off. */

enum { PG_SING, PG_CHOIR, PG_AIR, PG_ENV, PG_TOUCH, NPAGES };
enum { T_ROUTE = 100, T_DEPTH, T_VEL };
static const char *const PAGE_NAME[NPAGES] = {"SING", "CHOIR", "AIR", "ENVELOPE", "TOUCH"};
static const uint8_t PAGE_P[NPAGES][4] = {
    {MK_VOWEL, MK_HEAD, MK_BREATH, MK_GLIDE},
    {MK_VOICES, MK_DETUNE, MK_SPREAD, MK_LEVEL},
    {MK_VIBRATO, MK_VIBRATE, MK_DELAY, MK_DELAYRATE},
    {MK_ATTACK, MK_DECAY, MK_SUSTAIN, MK_RELEASE},
    {T_ROUTE, T_DEPTH, T_VEL, MK_TUNE},
};
static const char *const PAGE_LABEL[NPAGES][4] = {
    {"VOWEL", "HEAD", "BREATH", "GLIDE"},
    {"VOICES", "DETUNE", "SPREAD", "LEVEL"},
    {"VIBRATO", "RATE", "ECHO", "TIME"},
    {"ATTACK", "DECAY", "SUSTAIN", "RELEASE"},
    {"PRESSURE", "DEPTH", "VELOCITY", "TUNE"},
};
static const char *const ROUTE_NAME[MK_NAT] = {"VOWEL", "PITCH", "BOTH", "BOTH INV"};

#define K_PANEL RGB(250, 244, 232)
#define K_LINE RGB(216, 205, 186)
#define K_DIM RGB(150, 136, 118)
#define K_TEXT RGB(72, 58, 48)
#define TAG_H 30
#define STRIP_Y ART_SCENE_H
#define STRIP_H (240 - ART_SCENE_H)

static struct {
    uint32_t btn, keys;
    uint8_t dirty;
    uint32_t act_t, saved_t;
    char msg[24];
    uint32_t msg_until;
    int8_t touched;
    /* the screen */
    uint8_t scene;                     /* the character on screen (0xFF: none yet) */
    face_pose_t pose;
    uint8_t pose_ok;
    uint32_t strip_until, tag_until;
    uint8_t strip_on, tag_on, strip_dirty, tag_dirty;
    uint32_t last_draw;
    face_idle_t idle;
    uint32_t idle_t;
    /* playing */
    uint8_t stack[NKEYS], nstack;      /* held keys, oldest first: the last sounds */
    int16_t hold_note;                 /* HOLD: the latched note (-1: none) */
    uint8_t hold;
    uint8_t sung;                      /* a note is sounding (keys, HOLD or MIDI in) */
    uint8_t midi_notes;                /* notes held from MIDI in */
    uint32_t glo_t;
} ui;

static void say(const char *a)
{
    uint32_t i;
    for (i = 0; i < 23u && a && a[i]; i++)
        ui.msg[i] = a[i];
    ui.msg[i] = 0;
    ui.msg_until = plat_ms() + 1500u;
    ui.tag_until = ui.msg_until;
    ui.tag_dirty = 1;
}
void ui_say(const char *a, const char *b)
{
    char t[24];
    uint32_t i = 0, j;
    for (j = 0; a && a[j] && i < 22u; j++)
        t[i++] = a[j];
    if (b && i < 21u)
        t[i++] = ' ';
    for (j = 0; b && b[j] && i < 23u; j++)
        t[i++] = b[j];
    t[i] = 0;
    say(t);
}

static char *itoa_u(uint32_t v, char *b)
{
    char t[12];
    int n = 0;
    do {
        t[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v);
    while (n)
        *b++ = t[--n];
    *b = 0;
    return b;
}
static char *str_cp(char *b, const char *s)
{
    while (*s)
        *b++ = *s++;
    *b = 0;
    return b;
}

static void mark_dirty(void) { ui.dirty = 1; }
static const art_char_t *cur(void) { return &ART[proj.character]; }
static float *cur_p(void) { return proj.p[proj.character]; }

/* --------------------------------------------------------------------------- playing */
static int note_of(uint32_t key) { return 53 + 12 * ((int)proj.octave - 2) + (int)key; }

static void sing_on(int n, uint32_t vel)
{
    if (n < 0 || n > 127)
        return;
    monk_note_on(n, (int)vel);
    plat_midi3(0x90, (uint32_t)n, vel);
}
static void sing_off(int n)
{
    if (n < 0 || n > 127)
        return;
    monk_note_off(n);
    plat_midi3(0x80, (uint32_t)n, 64);
}

static void stack_remove(uint32_t key)
{
    uint32_t i, j = 0;
    for (i = 0; i < ui.nstack; i++)
        if (ui.stack[i] != key)
            ui.stack[j++] = ui.stack[i];
    ui.nstack = (uint8_t)j;
}

static void key_down(uint32_t key, uint32_t vel)
{
    int n = note_of(key);
    stack_remove(key);
    if (ui.nstack < NKEYS)
        ui.stack[ui.nstack++] = (uint8_t)key;
    if (ui.hold) {
        if (ui.hold_note >= 0 && ui.hold_note != n)
            sing_off(ui.hold_note);
        ui.hold_note = (int16_t)n;
    }
    sing_on(n, vel);
}

static void key_up(uint32_t key)
{
    stack_remove(key);
    if (ui.hold)
        return;                        /* HOLD: the note stays (the engine's stack keeps it) */
    sing_off(note_of(key));
}

static void set_hold(int on)
{
    ui.hold = (uint8_t)on;
    if (on) {
        ui.hold_note = ui.nstack ? (int16_t)note_of(ui.stack[ui.nstack - 1u]) : -1;
        say("HOLD");
    } else {
        uint32_t k;
        if (ui.hold_note >= 0) {
            int still = 0;
            for (k = 0; k < ui.nstack; k++)
                if (note_of(ui.stack[k]) == ui.hold_note)
                    still = 1;
            if (!still)
                sing_off(ui.hold_note);
        }
        ui.hold_note = -1;
        say("HOLD OFF");
    }
}

static void all_off(void)
{
    uint32_t k;
    for (k = 0; k < ui.nstack; k++)
        sing_off(note_of(ui.stack[k]));
    if (ui.hold_note >= 0)
        sing_off(ui.hold_note);
    ui.nstack = 0;
    ui.hold_note = -1;
    ui.midi_notes = 0;
    monk_panic();
}

/* the velocity of a top key (no converter): the PA10 probe, else 100 */
static uint32_t top_vel(uint32_t key)
{
    uint32_t v;
    if (key >= SN_TOP0 && (v = sense_probe_velocity(key)) != 0)
        return v;
    return 100;
}

/* pressure: every 5 ms, each held analog key's (smoothed over ~30 ms: a lift is a few fast samples
 * and must not register); the sounding key's drives the voice, every key's goes out as poly AT */
static uint16_t at_smooth[NKEYS];
static uint8_t at_sent[NKEYS];
static uint32_t at_t;
static uint8_t at_voice;
static void pressure(void)
{
    uint32_t i, top = ui.nstack ? ui.stack[ui.nstack - 1u] : 0xFFu;
    uint8_t pv = 0;
    if (plat_ms() - at_t < 5u)
        return;
    at_t = plat_ms();
    for (i = 0; i < NKEYS; i++) {
        uint8_t p;
        if (!sense_is_analog(i))
            continue;
        if (sn_key[i].st != SN_DOWN) {
            at_smooth[i] = 0;
            at_sent[i] = 0;
            continue;
        }
        at_smooth[i] = (uint16_t)(at_smooth[i] + ((int32_t)sense_pressure(i) * 16 - (int32_t)at_smooth[i]) / 6);
        p = (uint8_t)(at_smooth[i] / 16u);
        if (i == top)
            pv = p;
        if (p != at_sent[i]) {
            int n = note_of(i);
            at_sent[i] = p;
            if (n >= 0 && n <= 127)
                plat_midi3(0xA0, (uint32_t)n, p);
        }
    }
    if (pv != at_voice) {
        at_voice = pv;
        monk_pressure((float)pv * (1.0f / 127.0f));
    }
}

static void midi_in(void)
{
    uint32_t pkt;
    while (plat_midi_in(&pkt)) {
        uint32_t cin = pkt & 0xFu, st = (pkt >> 8) & 0xF0u, d1 = (pkt >> 16) & 0x7Fu, d2 = (pkt >> 24) & 0x7Fu;
        if (cin == 0x9u && st == 0x90u && d2) {
            monk_note_on((int)d1, (int)d2);
            ui.midi_notes++;
        } else if ((cin == 0x8u && st == 0x80u) || (cin == 0x9u && st == 0x90u)) {
            monk_note_off((int)d1);
            if (ui.midi_notes)
                ui.midi_notes--;
        } else if ((cin == 0xAu && st == 0xA0u) || (cin == 0xDu && st == 0xD0u)) {
            monk_pressure((float)(st == 0xA0u ? d2 : d1) * (1.0f / 127.0f));
        } else if (cin == 0xBu && st == 0xB0u) {
            int id = d1 == 1u ? MK_VOWEL : d1 == 5u ? MK_GLIDE : d1 == 7u ? MK_LEVEL : d1 == 12u ? MK_DELAY
                   : d1 == 13u ? MK_HEAD : -1;
            if (id >= 0) {
                cur_p()[id] = (float)d2 * (1.0f / 127.0f);
                monk_param(id, cur_p()[id]);
                mark_dirty();
            } else if (d1 == 123u || d1 == 120u) {
                all_off();
            }
        }
    }
}

static void events(void)
{
    sense_ev_t e;
    midi_in();
    pressure();
    while (sense_take(&e)) {
        if (e.type == EV_ON)
            key_down(e.key, e.vel);
        else if (e.type == EV_OFF)
            key_up(e.key);
    }
}

/* --------------------------------------------------------------------------- values */
static float pval(uint32_t page, uint32_t i)
{
    uint32_t id = PAGE_P[page][i];
    switch (id) {
    case T_ROUTE: return (float)proj.at_route;
    case T_DEPTH: return (float)proj.at_depth * 0.01f;
    case T_VEL: return (float)proj.vel_sens * 0.01f;
    default: return cur_p()[id];
    }
}

static void pfmt(uint32_t page, uint32_t i, char *b)
{
    uint32_t id = PAGE_P[page][i];
    float v = pval(page, i);
    char *p = b;
    switch (id) {
    case T_ROUTE: str_cp(b, ROUTE_NAME[proj.at_route]); return;
    case MK_VOICES: itoa_u((uint32_t)(v * 9.0f + 1.5f), b); return;
    case MK_DETUNE: str_cp(itoa_u((uint32_t)(v * 50.0f + 0.5f), b), " ct"); return;
    case MK_HEAD: str_cp(itoa_u((uint32_t)(v * 30.0f + 0.5f), b), " cm"); return;
    case MK_ATTACK:
    case MK_DECAY:
    case MK_RELEASE: {                 /* seconds, two decimals */
        uint32_t c = (uint32_t)(v * 500.0f + 0.5f);
        p = itoa_u(c / 100u, p);
        *p++ = '.';
        *p++ = (char)('0' + c / 10u % 10u);
        *p++ = (char)('0' + c % 10u);
        str_cp(p, " s");
        return;
    }
    case MK_TUNE: {
        int32_t st = (int32_t)(v * 24.0f + 0.5f) - 12;
        if (st > 0)
            *p++ = '+';
        else if (st < 0)
            *p++ = '-';
        str_cp(itoa_u((uint32_t)(st < 0 ? -st : st), p), " st");
        return;
    }
    default:
        itoa_u((uint32_t)(v * 100.0f + 0.5f), b);
        return;
    }
}

static void turn(uint32_t i, int32_t e)
{
    uint32_t id = PAGE_P[proj.page][i];
    float *p = cur_p(), step = id == MK_VOICES ? 1.0f / 9.0f : id == MK_TUNE ? 1.0f / 24.0f : 0.01f;
    if (e > 3 || e < -3)
        e *= 2;                        /* a fast turn goes further */
    switch (id) {
    case T_ROUTE: proj.at_route = (uint8_t)clamp((int32_t)proj.at_route + (e > 0 ? 1 : -1), 0, MK_NAT - 1); break;
    case T_DEPTH: proj.at_depth = (uint8_t)clamp((int32_t)proj.at_depth + e, 0, 100); break;
    case T_VEL: proj.vel_sens = (uint8_t)clamp((int32_t)proj.vel_sens + e, 0, 100); break;
    default: {
        float v = p[id] + step * (float)e;
        if (id == MK_VOICES || id == MK_TUNE)   /* on the steps */
            v = (float)(int32_t)(v / step + 0.5f) * step;
        p[id] = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
        monk_param((int)id, p[id]);
        break;
    }
    }
    if (id >= T_ROUTE)
        monk_touch(proj.at_route, (float)proj.at_depth * 0.01f, (float)proj.vel_sens * 0.01f);
    ui.touched = (int8_t)i;
    ui.strip_until = plat_ms() + 2500u;
    ui.strip_dirty = 1;
    mark_dirty();
}

static void set_page(uint32_t pg)
{
    proj.page = (uint8_t)(pg % NPAGES);
    ui.touched = -1;
    ui.strip_until = plat_ms() + 2500u;
    ui.strip_dirty = 1;
    ui.tag_until = plat_ms() + 1500u;
    ui.tag_dirty = 1;
    ui.msg_until = 0;
    mark_dirty();
}

static void set_character(int32_t c)
{
    c = (c % ART_N + ART_N) % ART_N;
    if ((uint32_t)c == proj.character)
        return;
    proj.character = (uint8_t)c;
    project_apply();
    ui.tag_until = plat_ms() + 2000u;
    ui.tag_dirty = 1;
    ui.strip_dirty = 1;
    ui.msg_until = 0;
    mark_dirty();
}

static void button(int b)
{
    switch (b) {
    case B_EDIT: set_page(proj.page + 1u); break;
    case B_HOME: set_page(PG_SING); break;
    case B_ENV: set_page(PG_ENV); break;
    case B_LFO:
    case B_FX: set_page(PG_AIR); break;
    case B_PLAY: set_hold(!ui.hold); break;
    case B_SAVE:
        if (project_save() == 0) {
            ui.dirty = 0;
            ui.saved_t = plat_ms();
            say("SAVED");
        } else {
            say("SAVE FAILED");
        }
        break;
    case B_GLO:
        if (plat_ms() - ui.glo_t < 2000u) {
            project_reset_character(proj.character);
            project_apply();
            ui.glo_t = 0;
            ui.strip_dirty = 1;
            mark_dirty();
            say("VOICE RESET");
        } else {
            ui.glo_t = plat_ms();
            say("GLO AGAIN: RESET VOICE");
        }
        break;
    case B_OCTDN:
    case B_OCTUP: {
        char t[12] = "OCTAVE ";
        int32_t o = (int32_t)proj.octave + (b == B_OCTUP ? 1 : -1);
        if (o < 0 || o > 4)
            break;
        proj.octave = (uint8_t)o;
        t[7] = o >= 2 ? '+' : '-';
        t[8] = (char)('0' + (o >= 2 ? o - 2 : 2 - o));
        t[9] = 0;
        say(t);
        mark_dirty();
        break;
    }
    default: break;
    }
}

static void input(void)
{
    uint32_t btn = plat_buttons(), keys = plat_keys(), ch, i;
    int32_t e;
    ch = btn ^ ui.btn;
    ui.btn = btn;
    for (i = 0; i < NB; i++)
        if ((ch & (1u << i)) && (btn & (1u << i)))
            button((int)i);
    ch = keys ^ ui.keys;
    ui.keys = keys;
    for (i = 0; i < NKEYS; i++) {              /* the keys without an analog row: the digital edge */
        if (!(ch >> i & 1u) || sense_is_analog(i))
            continue;
        if (keys >> i & 1u)
            key_down(i, top_vel(i));
        else
            key_up(i);
    }
    events();
    if (btn || keys || ch)
        ui.act_t = plat_ms();
    if ((e = plat_enc(EN_SELECT)) != 0)
        set_page((uint32_t)((int32_t)proj.page + NPAGES + (e > 0 ? 1 : -1)));
    if ((e = plat_enc(EN_PRESET)) != 0)
        set_character((int32_t)proj.character + (e > 0 ? 1 : -1));
    if ((e = plat_enc(EN_ALGO)) != 0) {        /* the vowel, wherever the page is */
        float *p = cur_p();
        p[MK_VOWEL] = fm_clampf(p[MK_VOWEL] + 0.02f * (float)e, 0.0f, 1.0f);
        monk_param(MK_VOWEL, p[MK_VOWEL]);
        if (proj.page == PG_SING)
            ui.strip_dirty = 1;
        mark_dirty();
        ui.act_t = plat_ms();
    }
    for (i = 0; i < 4u; i++)
        if ((e = plat_enc(EN_K1 + (int)i)) != 0) {
            turn(i, e);
            ui.act_t = plat_ms();
        }
    ui.sung = ui.nstack || ui.hold_note >= 0 || ui.midi_notes || monk_ui_active;
}

/* --------------------------------------------------------------------------- screen */
static void text_c(int32_t cx, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)
{
    cv_text(cx - text_w(f, s) / 2, y, f, s, c);
}

/* the canvas's pixels toward colour c by a/256 (a panel over the scene) */
static void cv_veil(uint16_t c, uint32_t a)
{
    uint32_t i, n = cv_w * cv_h;
    int32_t r = c >> 11, g = (c >> 5) & 63, b = c & 31;
    for (i = 0; i < n; i++) {
        uint32_t d = swap16(cv_px[i]);
        int32_t dr = (int32_t)(d >> 11), dg = (int32_t)((d >> 5) & 63u), db = (int32_t)(d & 31u);
        dr += (r - dr) * (int32_t)a / 256;
        dg += (g - dg) * (int32_t)a / 256;
        db += (b - db) * (int32_t)a / 256;
        cv_px[i] = swap16(((uint32_t)dr << 11) | ((uint32_t)dg << 5) | (uint32_t)db);
    }
}

/* rows y0 .. y0 + h - 1 of the scene, with its face, into the canvas (not blitted) */
static void scene_rows(int32_t y0, int32_t h)
{
    const art_char_t *a = cur();
    cv_begin(240, (uint32_t)h, 0);
    face_scene(a, cv_px, 0, y0, 240, h);
    face_patch(a, (uint32_t)(ui.pose.blink ? 5u : 0u) + ui.pose.loud, cv_px, 0, y0, 240, h);
}

static void draw_region(void)
{
    const art_char_t *a = cur();
    cv_begin(a->rw, a->rh, 0);
    face_draw(a, ui.pose, cv_px);
    cv_blit(a->rx, a->ry);
}

static void draw_tag(void)
{
    char b[40], *p;
    const art_char_t *a = cur();
    scene_rows(0, TAG_H);
    if (ui.tag_on) {
        cv_veil(K_PANEL, 220);
        cv_rect(0, TAG_H - 2, 240, 2, a->accent);
        if (plat_ms() < ui.msg_until) {
            text_c(120, 7, &FONT_B, ui.msg, K_TEXT);
        } else {
            p = str_cp(b, a->name);
            cv_text(10, 7, &FONT_B, b, a->accent);
            (void)p;
            str_cp(b, PAGE_NAME[proj.page]);
            cv_text(230 - text_w(&FONT_S, b), 7, &FONT_S, b, K_DIM);
        }
    }
    cv_blit(0, 0);
}

static void draw_strip(void)
{
    uint32_t i;
    const art_char_t *a = cur();
    scene_rows(STRIP_Y, STRIP_H);
    if (ui.strip_on) {
        cv_veil(K_PANEL, 224);
        cv_rect(0, 0, 240, 2, a->accent);
        for (i = 0; i < 4u; i++) {
            char b[16];
            int32_t x = (int32_t)i * 60;
            uint16_t c = (int8_t)i == ui.touched ? a->accent : K_TEXT;
            if (i)
                cv_rect(x, 10, 1, STRIP_H - 18, K_LINE);
            text_c(x + 30, 7, &FONT_XS, PAGE_LABEL[proj.page][i], K_DIM);
            pfmt(proj.page, i, b);
            text_c(x + 30, 24, &FONT_B, b, c);
        }
    }
    cv_blit(0, STRIP_Y);
}

static face_pose_t pose_now(void)
{
    face_pose_t p;
    float v;
    if (ui.sung) {
        float l = monk_ui_amp;
        v = monk_ui_vowel;
        p.loud = (uint8_t)(fm_clampf(l, 0.0f, 1.0f) * 4.0f + 0.5f);
        p.blink = 0;
    } else {
        v = face_idle_vowel(&ui.idle);
        p.loud = 0;
        p.blink = (uint8_t)face_idle_blink(&ui.idle);
    }
    p.vowel = (uint8_t)(fm_clampf(v, 0.0f, 1.0f) * (float)(FACE_VOWEL_FRAMES - 1) + 0.5f);
    return p;
}

static void draw(void)
{
    uint32_t now = plat_ms();
    face_pose_t p;
    int full = ui.scene != proj.character, tag, strip;
    while (now - ui.idle_t >= 100u) {          /* the idle animation, 100 ms a tick, reset by singing */
        ui.idle_t += 100u;
        if (ui.sung)
            face_idle_reset(&ui.idle);
        else
            face_idle_advance(&ui.idle);
    }
    p = pose_now();
    tag = now < ui.tag_until;
    strip = now < ui.strip_until;
    if (full) {
        ui.scene = proj.character;
        ui.pose = p;
        scene_rows(0, 120);
        cv_blit(0, 0);
        scene_rows(120, 120);
        cv_blit(0, 120);
        draw_region();
        ui.tag_dirty = ui.strip_dirty = 1;
        ui.tag_on = ui.strip_on = 0;
    } else if (!ui.pose_ok || p.vowel != ui.pose.vowel || p.loud != ui.pose.loud || p.blink != ui.pose.blink) {
        ui.pose = p;
        draw_region();
    }
    ui.pose_ok = 1;
    if (tag != ui.tag_on || (tag && ui.tag_dirty) || (full && tag)) {
        ui.tag_on = (uint8_t)tag;
        draw_tag();
    }
    ui.tag_dirty = 0;
    if (strip != ui.strip_on || (strip && ui.strip_dirty) || (full && strip)) {
        ui.strip_on = (uint8_t)strip;
        draw_strip();
    }
    ui.strip_dirty = 0;
}

static void leds(void)
{
    uint32_t b = 0;
    if (ui.hold)
        b |= 1u << B_PLAY;
    if (proj.page == PG_SING)
        b |= 1u << B_HOME;
    if (proj.page == PG_ENV)
        b |= 1u << B_ENV;
    if (proj.page == PG_AIR)
        b |= (1u << B_LFO) | (1u << B_FX);
    plat_leds(b, 0);                   /* no key lights: an LED's current lifts the key's reading */
}

/* 4 s after the last touch, at most every 10 s */
static void autosave(void)
{
    uint32_t now = plat_ms();
    if (!ui.dirty || now - ui.act_t < 4000u || now - ui.saved_t < 10000u)
        return;
    if (project_save() == 0) {
        ui.dirty = 0;
        ui.saved_t = now;
    }
}

void ui_init(void)
{
    memset(&ui, 0, sizeof ui);
    ui.touched = -1;
    ui.scene = 0xFF;
    ui.hold_note = -1;
    ui.idle_t = plat_ms();
    ui.tag_until = plat_ms() + 2000u;
    face_idle_reset(&ui.idle);
}

void ui_frame(void)
{
    input();
    autosave();
    leds();
    if (plat_ms() - ui.last_draw < 33u)        /* ~30 frames/s: the mouth moves in frames anyway */
        return;
    ui.last_draw = plat_ms();
    draw();
}

void ui_input_only(void)
{
    input();
    leds();
}
