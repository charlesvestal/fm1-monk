/* SPDX-License-Identifier: GPL-3.0-only */
/* MONK-1 UI. Part of the unity build (after gfx.c, sense.c, project.c, inflate.c and face.c).
 *
 *   keys           sing (one voice: the newest key held sounds, letting go falls back to the one
 *                  under it). Pressing harder on a held key (the driven reading: Velo's sensing)
 *                  moves the vowel, or bends, or both (TOUCH: PRESSURE, DEPTH, SENSITIVE)
 *   PRESETS        the singer: twelve characters, each with its voice, each remembering its settings
 *   ALGORITHM      the vowel (the knob everyone plays with)
 *   page buttons   a set of knobs each, as Polly: HOME Sing, EDIT Choir, LFO Vibrato, FX Echo, ENV Envelope,
 *                  GLO Touch; a button opens its first page, SELECT turns a button's pages (the dots: which)
 *   KNOB 1-4       the page's knobs
 *   OCT- / OCT+    octave                       PLAY            HOLD: the note stays after the key
 *   SAVE           saves now (it saves itself a few seconds after the last change); held 1.5 s: this
 *                  singer's voice back to how it came
 *
 * The screen is the character on its stage (face.c): the mouth follows the vowel, in frames, and
 * swells with loudness; idle, it blinks and shuffles through its vowels as the iOS one does. A page
 * button, a knob or a new singer brings up the menu over it for a few seconds (Polly's layout): the
 * header (the button that opened the page as a keycap, the page, the singer), the tag (whose the knobs
 * are, the page dots), the knobs (name, value, a bar) and the encoders' line.
 * USB MIDI out: the notes (channel 1) with the keys' velocity, and polyphonic aftertouch. In: notes (any
 * channel), CC 1 vowel, 5 glide, 7 level, 12 echo, 13 head size, aftertouch, CC 123 all off. */

enum { PG_SING, PG_CHOIR, PG_VIB, PG_ECHO, PG_ENV, PG_TOUCH, NPAGES };
enum { T_ROUTE = 100, T_DEPTH, T_SENS, T_NONE = 255 };
static const char *const PAGE_NAME[NPAGES] = {"Sing", "Choir", "Vibrato", "Echo", "Envelope", "Touch"};
static const uint8_t PAGE_P[NPAGES][4] = {
    {MK_VOWEL, MK_HEAD, MK_BREATH, MK_GLIDE},
    {MK_VOICES, MK_DETUNE, MK_SPREAD, MK_LEVEL},
    {MK_VIBRATO, MK_VIBRATE, T_NONE, T_NONE},
    {MK_DELAY, MK_DELAYRATE, T_NONE, T_NONE},
    {MK_ATTACK, MK_DECAY, MK_SUSTAIN, MK_RELEASE},
    {T_ROUTE, T_DEPTH, T_SENS, MK_TUNE},
};
static const char *const PAGE_LABEL[NPAGES][4] = {
    {"Vowel", "Head", "Breath", "Glide"},
    {"Voices", "Detune", "Spread", "Level"},
    {"Depth", "Rate", 0, 0},
    {"Amount", "Time", 0, 0},
    {"Attack", "Decay", "Sustain", "Release"},
    {"Pressure", "Depth", "Sensitive", "Tune"},
};
/* the page buttons: one set of knobs each, and each set one button's; a button opens its first page, SELECT
 * turns its pages (n > 1: the tag's dots) */
static const struct { uint8_t btn, first, n; } PAGE_BTN[] = {
    {B_HOME, PG_SING, 1}, {B_EDIT, PG_CHOIR, 1}, {B_LFO, PG_VIB, 1}, {B_FX, PG_ECHO, 1}, {B_ENV, PG_ENV, 1},
    {B_GLO, PG_TOUCH, 1}};
#define NPAGE_BTN (sizeof PAGE_BTN / sizeof PAGE_BTN[0])
static const char *const BTN_LABEL[NB] = {"FX", "SEL", "ENV", "LFO", "EDIT", "GLO", "HOME", "SAVE", "ARP", "SEQ",
                                          "PLAY", "REC", "OCT-", "OCT+"};
static uint32_t page_group(uint32_t pg)
{
    uint32_t i;
    for (i = 0; i < NPAGE_BTN; i++)
        if (pg >= PAGE_BTN[i].first && pg < (uint32_t)(PAGE_BTN[i].first + PAGE_BTN[i].n))
            return i;
    return 0;
}
static const char *const ROUTE_NAME[MK_NAT] = {"VOWEL", "PITCH", "BOTH", "BOTH INV"};

#define K_PANEL RGB(250, 244, 232)
#define K_LINE RGB(216, 205, 186)
#define K_DIM RGB(112, 98, 84)         /* Polly's: 5.6:1 on the cream */
#define K_FAINT RGB(150, 136, 118)
#define K_BG RGB(250, 244, 232)
#define K_TEXT RGB(72, 58, 48)
#define K_PANEL2 RGB(236, 228, 212)
#define TAG_H 24                       /* the header */
#define STRIP_Y 160                    /* the tag, the knobs, the encoders' line */
#define STRIP_H (240 - STRIP_Y)

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
    uint32_t on[4];                    /* the notes the keys have on in the voice (bit n: note n) */
    uint32_t save_t;                   /* SAVE pressed at (0: not held) */
    uint8_t save_long;                 /* its hold did the reset */
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
static void menu_show(uint32_t ms)     /* the menu over the scene: header, tag, knobs, encoders */
{
    ui.strip_until = ui.tag_until = plat_ms() + ms;
    ui.strip_dirty = ui.tag_dirty = 1;
}
static const art_char_t *cur(void) { return &ART[proj.character]; }
static float *cur_p(void) { return proj.p[proj.character]; }

/* --------------------------------------------------------------------------- playing */
static int note_of(uint32_t key) { return 53 + 12 * ((int)proj.octave - 2) + (int)key; }

static void sing_on(int n, uint32_t vel)
{
    if (n < 0 || n > 127)
        return;
    ui.on[n >> 5] |= 1u << (n & 31);
    monk_note_on(n, (int)vel);
    plat_midi3(0x90, (uint32_t)n, vel);
}
static void sing_off(int n)
{
    if (n < 0 || n > 127)
        return;
    ui.on[n >> 5] &= ~(1u << (n & 31));
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

static int key_held_note(int n)       /* a key held now plays note n */
{
    uint32_t k;
    for (k = 0; k < ui.nstack; k++)
        if (note_of(ui.stack[k]) == n)
            return 1;
    return 0;
}

/* every note on that no key holds and HOLD does not keep goes off (a key's own release, or one missed:
 * nothing may be left singing on its own) */
static void release_strays(void)
{
    int n;
    for (n = 0; n < 128; n++)
        if ((ui.on[n >> 5] >> (n & 31) & 1u) && !key_held_note(n) && !(ui.hold && n == ui.hold_note))
            sing_off(n);
}

static void key_up(uint32_t key)
{
    stack_remove(key);
    release_strays();                  /* HOLD keeps its one note, the latched one */
}

static void set_hold(int on)
{
    ui.hold = (uint8_t)on;
    if (on) {
        ui.hold_note = ui.nstack ? (int16_t)note_of(ui.stack[ui.nstack - 1u]) : -1;
        say("HOLD");
    } else {
        ui.hold_note = -1;
        release_strays();
        say("HOLD OFF");
    }
}

static void all_off(void)
{
    ui.nstack = 0;
    ui.hold_note = -1;
    release_strays();
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

/* pressure: every 5 ms, each held analog key's; the sounding key's drives the voice, every key's goes
 * out as poly AT. Counted from where the key RESTS, not from where it landed: a key goes on settling
 * after the note (measured: resting on a key read 100 within 150 ms when the zero was the landing).
 * So the zero follows the driven reading down for P_SETTLE ms, then creeps after it (a few seconds),
 * and comes back up at once when the key is eased. The lean is the pill's resistance below its rest on
 * a log scale (Velo's: the reading less the diode, SN_DBASE): P_DEAD of an octave is noise, P_SPAN more (SENSITIVE)
 * is 127 (a light rest to a firm press measured ~0.7 octave at DRIVE 2). Smoothed over ~30 ms. */
#define P_SETTLE 250u                  /* ms */
#define P_DEAD 0.04f                   /* octaves of resistance */
#define P_SPAN(s) (0.8f * (1.0f - 0.85f * (s)))   /* SENSITIVE 0..1: 0.8 .. 0.12 octave; 50: 0.46 */
static float p_base[NKEYS], p_s[NKEYS];
static uint32_t p_t0[NKEYS];
static uint8_t at_sent[NKEYS], p_on[NKEYS];
static uint32_t at_t;
static uint8_t at_voice;
static void pressure(void)
{
    uint32_t i, now = plat_ms(), top = ui.nstack ? ui.stack[ui.nstack - 1u] : 0xFFu;
    uint8_t pv = 0;
    if (now - at_t < 5u)
        return;
    at_t = now;
    for (i = 0; i < NKEYS; i++) {
        float d, lean, x;
        uint8_t p;
        if (!sense_is_analog(i))
            continue;
        if (sn_key[i].st != SN_DOWN || !sn_key[i].dn) {
            p_on[i] = 0;
            p_s[i] = 0.0f;
            at_sent[i] = 0;
            continue;
        }
        d = (float)(sn_key[i].d > SN_DBASE + 1u ? sn_key[i].d - SN_DBASE : 1u);
        if (!p_on[i]) {
            p_on[i] = 1;
            p_base[i] = d;
            p_t0[i] = now;
        }
        if (d > p_base[i])
            p_base[i] = d;                                     /* eased off: that is the rest now */
        else
            p_base[i] += (d - p_base[i]) * (now - p_t0[i] < P_SETTLE ? 0.25f : 0.0008f);
        lean = fm_log2f(p_base[i] / d);
        x = fm_clampf((lean - P_DEAD) / P_SPAN((float)proj.p_sens * 0.01f), 0.0f, 1.0f);
        p_s[i] += (x - p_s[i]) * 0.17f;
        p = (uint8_t)(p_s[i] * 127.0f + 0.5f);
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
    case T_NONE: return 0.0f;
    case T_ROUTE: return (float)proj.at_route / (float)(MK_NAT - 1);
    case T_DEPTH: return (float)proj.at_depth * 0.01f;
    case T_SENS: return (float)proj.p_sens * 0.01f;
    default: return cur_p()[id];
    }
}

static void pfmt(uint32_t page, uint32_t i, char *b)
{
    uint32_t id = PAGE_P[page][i];
    float v = pval(page, i);
    char *p = b;
    switch (id) {
    case T_NONE: *b = 0; return;
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
    if (id == T_NONE)
        return;
    if (e > 3 || e < -3)
        e *= 2;                        /* a fast turn goes further */
    switch (id) {
    case T_ROUTE: proj.at_route = (uint8_t)clamp((int32_t)proj.at_route + (e > 0 ? 1 : -1), 0, MK_NAT - 1); break;
    case T_DEPTH: proj.at_depth = (uint8_t)clamp((int32_t)proj.at_depth + e, 0, 100); break;
    case T_SENS: proj.p_sens = (uint8_t)clamp((int32_t)proj.p_sens + e, 0, 100); break;
    default: {
        float v = p[id] + step * (float)e;
        if (id == MK_VOICES || id == MK_TUNE)   /* on the steps */
            v = (float)(int32_t)(v / step + 0.5f) * step;
        p[id] = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
        monk_param((int)id, p[id]);
        break;
    }
    }
    if (id >= T_ROUTE && id != T_NONE)
        monk_touch(proj.at_route, (float)proj.at_depth * 0.01f, 0.0f);
    ui.touched = (int8_t)i;
    menu_show(2500u);
    mark_dirty();
}

static void set_page(uint32_t pg)
{
    proj.page = (uint8_t)(pg % NPAGES);
    ui.touched = -1;
    ui.msg_until = 0;
    menu_show(3000u);
    mark_dirty();
}

static void set_character(int32_t c)
{
    c = (c % ART_N + ART_N) % ART_N;
    if ((uint32_t)c == proj.character)
        return;
    proj.character = (uint8_t)c;
    project_apply();
    ui.msg_until = 0;
    ui.touched = -1;
    menu_show(2500u);
    mark_dirty();
}

static void reset_voice(void)
{
    project_reset_character(proj.character);
    project_apply();
    ui.strip_dirty = 1;
    mark_dirty();
    say("VOICE RESET");
}

static void button(int b)
{
    uint32_t i;
    for (i = 0; i < NPAGE_BTN; i++)
        if (PAGE_BTN[i].btn == b) {        /* a page button: its first page */
            set_page(PAGE_BTN[i].first);
            return;
        }
    switch (b) {
    case B_PLAY: set_hold(!ui.hold); break;
    case B_SAVE:                           /* on its release (held 1.5 s: the reset, input()) */
        if (project_save() == 0) {
            ui.dirty = 0;
            ui.saved_t = plat_ms();
            say("SAVED");
        } else {
            say("SAVE FAILED");
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
    for (i = 0; i < NB; i++) {
        if (!(ch & (1u << i)))
            continue;
        if (i == B_SAVE) {                 /* SAVE: a tap saves, held 1.5 s resets the voice */
            if (btn & (1u << i)) {
                ui.save_t = plat_ms();
                ui.save_long = 0;
            } else {
                if (!ui.save_long)
                    button((int)i);
                ui.save_t = 0;
            }
        } else if (btn & (1u << i)) {
            button((int)i);
        }
    }
    if (ui.save_t && !ui.save_long && plat_ms() - ui.save_t >= 1500u) {
        ui.save_long = 1;
        reset_voice();
    }
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
    if ((e = plat_enc(EN_SELECT)) != 0) {   /* the button's pages */
        uint32_t g = page_group(proj.page), n = PAGE_BTN[g].n;
        if (n > 1)
            set_page(PAGE_BTN[g].first + (proj.page - PAGE_BTN[g].first + n + (e > 0 ? 1u : n - 1u)) % n);
        else
            menu_show(2000u);
    }
    if ((e = plat_enc(EN_PRESET)) != 0)
        set_character((int32_t)proj.character + (e > 0 ? 1 : -1));
    if ((e = plat_enc(EN_ALGO)) != 0) {        /* the vowel, wherever the page is */
        float *p = cur_p();
        p[MK_VOWEL] = fm_clampf(p[MK_VOWEL] + 0.02f * (float)e, 0.0f, 1.0f);
        monk_param(MK_VOWEL, p[MK_VOWEL]);
        if (proj.page == PG_SING)
            menu_show(2500u);
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
    uint32_t h = a->rh;
    if (ui.strip_on && a->ry + h > STRIP_Y)    /* the menu stays over it */
        h = STRIP_Y > a->ry ? STRIP_Y - a->ry : 0u;
    cv_begin(a->rw, a->rh, 0);
    face_draw(a, ui.pose, cv_px);
    if (h)
        lcd_blit(a->rx, a->ry, a->rw, h, cv_px);
}

static void cv_round(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t c)   /* Polly's */
{
    static const uint8_t IN[7][6] = {{0}, {1}, {1, 0}, {2, 1, 0}, {2, 1, 0, 0}, {3, 2, 1, 0, 0}, {4, 2, 1, 1, 0, 0}};
    int32_t j;
    if (r > 6)
        r = 6;
    if (r > h / 2)
        r = h / 2;
    cv_rect(x, y + r, w, h - 2 * r, c);
    for (j = 0; j < r; j++) {
        int32_t d = IN[r][j];
        cv_rect(x + d, y + j, w - 2 * d, 1, c);
        cv_rect(x + d, y + h - 1 - j, w - 2 * d, 1, c);
    }
}
/* the y to draw f at so its capitals sit centred in rows top .. top + h - 1 (Polly's) */
static int32_t cap_y(int32_t top, int32_t h, const felucca_font_t *f)
{
    int32_t c2 = f == &FONT_L ? 28 : f == &FONT_M ? 21 : 14;
    return (2 * top + h - 1 - c2) >> 1;
}
static int upper_only(const char *s)
{
    while (*s)
        if ((uint8_t)*s++ > 95u)
            return 0;
    return 1;
}
/* a keycap: the button's name in a small outlined box; returns its end */
static int32_t keycap(int32_t x, int32_t y, const char *bn)
{
    int32_t w = text_w(&FONT_XS, bn) + 6;
    cv_round(x, y, w, 13, 2, K_DIM);
    cv_round(x + 1, y + 1, w - 2, 11, 1, K_PANEL);
    cv_text(x + 3, cap_y(y, 13, &FONT_XS), &FONT_XS, bn, K_DIM);
    return x + w;
}

static int edited(void)                /* this singer's voice is not as it came */
{
    uint32_t k;
    for (k = 0; k < MK_NPARAM; k++) {
        float d = cur_p()[k] - cur()->p[k];
        if (d > 1e-4f || d < -1e-4f)
            return 1;
    }
    return 0;
}

/* the header: the button that opened the page (a keycap), the page; the singer at the right. A message replaces it */
static void draw_tag(void)
{
    const art_char_t *a = cur();
    scene_rows(0, TAG_H);
    if (ui.tag_on) {
        cv_veil(K_PANEL, 232);
        cv_rect(0, TAG_H - 1, 240, 1, K_LINE);
        if (plat_ms() < ui.msg_until) {
            text_c(120, cap_y(0, TAG_H, &FONT_B), &FONT_B, ui.msg, K_TEXT);
        } else {
            int32_t x = keycap(8, 6, BTN_LABEL[PAGE_BTN[page_group(proj.page)].btn]) + 6;
            cv_text(x, cap_y(0, TAG_H, &FONT_B), &FONT_B, PAGE_NAME[proj.page], K_TEXT);
            cv_text(232 - text_w(&FONT_B, a->name), cap_y(0, TAG_H, &FONT_B), &FONT_B, a->name, a->accent);
        }
    }
    cv_blit(0, 0);
}

/* an encoder's job: its name as an outlined pill, the job after it; returns the end */
static int32_t enc_job(int32_t x, int32_t y, const char *enc, const char *job)
{
    int32_t w = text_w(&FONT_XS, enc) + 10;
    cv_round(x, y, w, 14, 5, K_DIM);
    cv_round(x + 1, y + 1, w - 2, 12, 4, K_BG);
    cv_text(x + 5, cap_y(y, 14, &FONT_XS), &FONT_XS, enc, K_TEXT);
    return cv_text(x + w + 4, cap_y(y, 14, &FONT_XS), &FONT_XS, job, K_DIM) + 10;
}

/* the menu's lower part: the tag (whose the knobs are; the page dots), the knobs (name, value, a bar), the
 * encoders' line */
static void draw_strip(void)
{
    uint32_t i, g = page_group(proj.page), n = PAGE_BTN[g].n;
    const art_char_t *a = cur();
    scene_rows(STRIP_Y, STRIP_H);
    if (ui.strip_on) {
        char t[32];
        int32_t r = 232, w;
        cv_veil(K_BG, 236);
        if (n > 1) {                       /* which of the button's pages */
            uint32_t d = proj.page - PAGE_BTN[g].first;
            r -= (int32_t)n * 9 - 3;
            for (i = 0; i < n; i++)
                cv_round(r + (int32_t)i * 9, 4, 6, 6, 3, i == d ? K_TEXT : K_LINE);
            r -= 8;
        }
        if (proj.page == PG_TOUCH)
            str_cp(t, "all singers");
        else
            str_cp(str_cp(t, a->name), edited() ? " (edited)" : "");
        w = text_w(&FONT_XS, t);
        cv_text(r - w, -1, &FONT_XS, t, K_TEXT);
        if (r - w - 16 >= 24)
            cv_rect(8, 6, r - w - 16, 1, K_LINE);
        cv_round(4, 14, 232, 48, 6, K_PANEL2);
        for (i = 0; i < 4u; i++) {
            char b[16];
            int32_t x = (int32_t)i * 60, cx = x + 30;
            int hot = (int8_t)i == ui.touched;
            const char *nm = PAGE_LABEL[proj.page][i];
            if (i)
                cv_rect(x, 24, 1, 28, K_LINE);
            if (!nm)
                continue;
            text_c(cx, 14, &FONT_XS, nm, hot ? K_TEXT : K_DIM);
            pfmt(proj.page, i, b);
            if (upper_only(b) && text_w(&FONT_M, b) <= 56)
                text_c(cx, cap_y(27, 22, &FONT_M), &FONT_M, b, hot ? a->accent : K_TEXT);
            else
                text_c(cx, cap_y(27, 22, &FONT_B), &FONT_B, b, hot ? a->accent : K_TEXT);
            {
                int32_t bw = (int32_t)(44.0f * pval(proj.page, i) + 0.5f);
                cv_round(cx - 22, 52, 44, 6, 3, K_LINE);
                if (bw > 0)
                    cv_round(cx - 22, 52, bw < 6 ? 6 : bw, 6, 3, hot ? a->accent : K_FAINT);
            }
        }
        {
            int32_t x = 8;
            if (n > 1)
                x = enc_job(x, 64, "SELECT", "page");
            x = enc_job(x, 64, "PRESETS", "singer");
            enc_job(x, 64, "ALGO", "vowel");
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
        int closed = ui.strip_on && !strip;
        ui.strip_on = (uint8_t)strip;
        draw_strip();
        if (closed)
            draw_region();                 /* the mouth the menu covered */
    }
    ui.strip_dirty = 0;
}

static void leds(void)
{
    uint32_t b = 0;
    if (ui.hold)
        b |= 1u << B_PLAY;
    b |= 1u << PAGE_BTN[page_group(proj.page)].btn;   /* the page's button */
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
    ui.tag_until = ui.strip_until = plat_ms() + 2500u;
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
