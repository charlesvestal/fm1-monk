/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* FM-1 input HAL: key/button/encoder matrix and LEDs.
 *
 * One 11-column x 6-row diode matrix behind a 2x74HC595 chain (PA4 SER, PA3
 * SRCLK, PA1 RCLK), bit-banged and polled; rows PA0, PA5..PA8, PB7 with
 * pull-ups (low = closed). LED lines PH6/PH9/PA9/PA10 light the LED on the
 * same column, row PA7/PA8/PA5/PA6 respectively.
 *
 *   fm1_input_init();
 *   polled:  for (;;) { fm1_input_scan(); ... }
 *   IRQ:     call fm1_input_tick() from a ~10 kHz timer ISR; it advances one
 *            column per call (rows are sampled one tick after the column was
 *            latched, LEDs stay lit in between) and processes a frame every
 *            FM1_NCOL ticks. The main loop reads fm1_in.notes / buttons and
 *            takes edges/steps with fm1_input_edges() / fm1_enc_take().
 *
 * fm1_input_scan() runs one full frame (11 columns, ~0.6 ms) and calls
 * FM1_INPUT_IDLE() while it waits.
 * Keys/buttons: integrating debounce of FM1_DEBOUNCE frames.
 * Encoders: quadrature decoder (2-sample filter, + = clockwise) with detent
 * counting: states an encoder rests in for >= FM1_REST_FRAMES are learned, and
 * a step is emitted only on reaching a rest state after >= 2 net transitions.
 * One click = one step at any speed, whether a detent is a half or a full
 * quadrature cycle. fm1_enc_take() returns the steps.
 * LEDs: set fm1_led[col] (packed row bits, bit1 PA5..bit4 PA8); they are lit
 * while that column is selected. fm1_led_key/btn helpers address them by id.
 */
#pragma once
#include <stdint.h>
#include "fm1_time.h"
#include "fm1_gpio.h"

#include "fm1_adc.h"

#ifndef FM1_INPUT_IDLE
#define FM1_INPUT_IDLE() ((void)0)
#endif
/* Analog rows (fm1-velocity): the tick also starts an ADC conversion on row PA7 (even frames) or
 * PA8 (odd frames) of the column it has just latched, and takes the result at the next tick, so
 * every key on those two rows is sampled once per two frames (~2.2 ms): the voltage across the
 * key's carbon pill against the row's pull-up, 1023 open, falling as the pill is pressed harder.
 * FM1_INPUT_ANALOG(col, rowbit, v) is called from the tick with each sample (rowbit 3 = PA7,
 * 4 = PA8; the key is FM1_KEYMAP[rowbit][col]). The MASTER pot takes the slot of (odd frame,
 * column 0, PA8 = OCT-) and lands in fm1_an.master. */
#ifndef FM1_INPUT_ANALOG
#define FM1_INPUT_ANALOG(col, rowbit, v, driven) ((void)0)
#endif
/* Drive (fm1-velocity experiment): with fm1_an.drive set (1..4), a sample for which
 * FM1_INPUT_DRIVE(col, rowbit) is nonzero is taken with the row driven high as an output instead
 * of pulled up: the pin's own driver is then the divider's upper half, where the 10k pull-up only
 * sees the matrix diode's drop. Strength fm1_an.drive - 1 goes to HD0 (bit 0) and HD (bit 1), the
 * SDK's gpio_set_hd_opt(0..3) encoding: HD0 = gpio_set_hd, "the internal resistor off, a stronger
 * output" (clear, the default: the resistor in); HD = gpio_set_hd1, "more current". The datasheet
 * gives the PA pins 24 / 16 / 8 / 2.4 mA; which code is which is not documented (DRIVE 1, both
 * clear, should be the weakest). A driver weaker than the seated pill's pull is current-limited:
 * the pad then reads the diode plus that current through the pill and the 595, an ohmmeter
 * (2.4 mA across 100..300 ohms is 0.24..0.72 V, ~75..220 counts). The strong levels hold the pad
 * near 3.3 V and read nothing. The row is an input with its pull-up again at the next tick, before
 * the rows are read. Current: at most the level's, for one tick, on the one column latched. */
#ifndef FM1_INPUT_DRIVE
#define FM1_INPUT_DRIVE(col, rowbit) 0
#endif
/* Probe (fm1-velocity experiment): fm1_an.probe is a mask of rows to probe (bit 0 PA6, bit 1 PA7,
 * bit 2 PA8); each set row takes one frame of every 2 + (rows set), in which PA10 is sampled
 * instead of PA7 / PA8. Described for the top row, PA6, the reason for it (rows PA7 / PA8 the
 * same, to compare the probe with their direct reading): PA10 is the LED line of row PA6, whose keys (notes 20..26, columns 0..6)
 * have no converter: for that tick PA10 is an input with its pull-up (the LED in column n then
 * passes a little current into the column, so PA10 reads the column's low level plus the LED's
 * drop) and PA6 is driven high, so a pressed PA6 key sends a current through its pill into the
 * column and lifts the 595 output's low level: the harder the press, the more (PA6 at the weakest
 * drive, whatever fm1_an.drive is: the probe's scale depends on it). Columns 7..10 of
 * PA6 are buttons and are not probed, nor PA8's OCT- / OCT+. FM1_INPUT_PROBE(col, rowbit, v,
 * pressed) gets each reading, with the digital scan's last view of that key. */
#ifndef FM1_INPUT_PROBE
#define FM1_INPUT_PROBE(col, rowbit, v, pressed) ((void)0)
#endif
#define FM1_ADC_LED_PA10 2u
#define FM1_PROBE_COLS 7u
#ifndef FM1_LED_US
#define FM1_LED_US 40u           /* LED on-time per column (brightness vs scan rate) */
#endif
#define FM1_DEBOUNCE 8u           /* frames (~0.6 ms each) */
#define FM1_SETTLE_US 10u
#define FM1_REST_FRAMES 40u       /* ~25 ms still = a detent position */
#define FM1_NCOL 11u
#define FM1_NKEY 41u              /* ids: 0..13 buttons, 14..40 note keys */
#define FM1_NENC 7u


/* key id at (physical column, packed row bit), -1 = none */
static const int8_t FM1_KEYMAP[6][FM1_NCOL] = {
    {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},          /* PA0: encoders */
    { 5, 11,  4, 10,  3,  9,  2,  8, -1, -1, -1},          /* PA5 */
    {34, 35, 36, 37, 38, 40, 39, 13,  7,  6, 12},          /* PA6 */
    {23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33},          /* PA7 */
    { 0,  1, 15, 14, 17, 16, 19, 18, 20, 21, 22},          /* PA8 */
    {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},          /* PB7: encoder 6 */
};
/* encoder i: A at (col, row bit), B at (col, row bit) */
static const uint8_t FM1_ENC[FM1_NENC][4] = {
    {0, 0, 1, 0}, {2, 0, 3, 0}, {8, 1, 9, 1}, {8, 0, 9, 0}, {6, 0, 7, 0}, {4, 0, 5, 0}, {0, 5, 1, 5},
};
enum { FM1_BTN_OCT_DOWN = 0, FM1_BTN_OCT_UP = 1 };

static const int8_t FM1_LED_PORT[6] = {-1, FM1_PA, FM1_PA, FM1_PH, FM1_PH, -1};
static const uint8_t FM1_LED_BIT[6] = {0, 9, 10, 6, 9, 0};

static volatile struct {
    uint32_t notes;              /* debounced: bit n = note key n (0 = F3 .. 26 = G5) */
    uint32_t buttons;            /* debounced: bit i = button i (0..13) */
    uint32_t pressed, released;  /* button edges since the last fm1_input_edges() */
    uint32_t notes_pressed;      /* note-key press edges since the last fm1_input_note_edges() */
    uint8_t raw[FM1_NCOL];       /* last frame, packed rows, 1 = closed */
    uint8_t cnt[FM1_NKEY];
    uint8_t enc_prev[FM1_NENC], enc_last[FM1_NENC];
    uint8_t enc_rest[FM1_NENC];  /* learned rest (detent) states, bit per state */
    uint8_t enc_still[FM1_NENC]; /* frames since the last state change */
    int8_t enc_sub[FM1_NENC];    /* net transitions since the last rest state */
    int8_t enc_dir[FM1_NENC];    /* the last valid transition: +1 / -1 (0: none yet) */
    int16_t enc_steps[FM1_NENC]; /* + = clockwise */
    uint32_t frames;
} fm1_in;
static uint8_t fm1_led[FM1_NCOL];
/* dim LEDs (FoMni's "glow"), lit on one scan frame in fm1_led_dim_div (0 = off) */
static uint8_t fm1_led_dim[FM1_NCOL];
static volatile uint8_t fm1_led_dim_div;
static uint8_t fm1__dim_frame;
/* MONK-1: a lit LED's current goes through its column line and lifts the readings of that column's keys
 * (Velo measured +25 counts): a key that is held would see its pressure jump when an LED in its column
 * changes. FM1_INPUT_LED_OK(col, rowbit) answers 0 for a key that is not open: its column's LEDs then
 * stay dark on the ticks that sample it (the other ticks light them, so the buttons barely dim). */
#ifndef FM1_INPUT_LED_OK
#define FM1_INPUT_LED_OK(col, rowbit) 1
#endif
static uint32_t fm1__leds(uint32_t n)
{
    return fm1_led[n] | (fm1_led_dim_div && fm1__dim_frame == 0u ? fm1_led_dim[n] : 0u);
}
static volatile struct {
    uint16_t con;                /* the conversion's clock / start delay (FM1_ADC_CON); 0 = analog off */
    uint8_t pend, col, rowbit;   /* a conversion runs for (col, rowbit); rowbit 0 = the pot */
    uint16_t master;             /* the MASTER pot, 0..1023 */
    uint16_t timeouts;           /* conversions not done after a tick (the settings too slow) */
    uint16_t us;                 /* a timed conversion: how long it took (fm1_an_time_req) */
    uint8_t time_req;            /* main loop: time the next pot conversion (spins in the tick) */
    uint8_t immediate;           /* take the result as soon as the conversion is done (spins in the tick) */
    uint8_t selftest;            /* drive the sampled row low for its conversion: it must read 0 */
    uint8_t drive;               /* 0 off; 1..4: driven samples (FM1_INPUT_DRIVE) at strength drive - 1 */
    uint8_t probe;               /* rows to probe through PA10: bit 0 PA6, bit 1 PA7, bit 2 PA8 */
    uint8_t driven;              /* the pending conversion's row is driven high (restore at the take) */
    uint8_t probing;             /* the pending conversion is the probe (PA10 / PA6 to restore) */
    uint8_t phase;               /* frame phase: 0 PA7, 1 PA8, 2.. the probed rows in turn */
} fm1_an;

static void fm1__led_lines(uint32_t rowmask)
{
    uint32_t r;
    for (r = 1; r < 5u; r++) {
        if (rowmask & (1u << r))
            FM1_PR(FM1_LED_PORT[r], FM1_OUT) |= 1u << FM1_LED_BIT[r];
        else
            FM1_PR(FM1_LED_PORT[r], FM1_OUT) &= ~(1u << FM1_LED_BIT[r]);
    }
}

static void fm1__sr_word(uint32_t w)
{
    uint32_t i;
    for (i = 0; i < 16u; i++) {
        if (w & (0x8000u >> i))
            FM1_PR(FM1_PA, FM1_OUT) |= 1u << 4;
        else
            FM1_PR(FM1_PA, FM1_OUT) &= ~(1u << 4);
        FM1_PR(FM1_PA, FM1_OUT) |= 1u << 3;     /* each SFR write is far slower than the 595 needs */
        FM1_PR(FM1_PA, FM1_OUT) &= ~(1u << 3);
    }
    FM1_PR(FM1_PA, FM1_OUT) |= 1u << 1;
    FM1_PR(FM1_PA, FM1_OUT) &= ~(1u << 1);
}

static uint32_t fm1__rows(void)
{
    uint32_t a = FM1_PR(FM1_PA, FM1_IN), b = FM1_PR(FM1_PB, FM1_IN);
    return (~((a & 1u) | ((a >> 4) & 0x1Eu) | ((b >> 2) & 0x20u))) & 0x3Fu;
}

static void fm1__wait(uint32_t us)
{
    uint32_t t0 = fm1_ticks(), span = us * FM1_TICKS_PER_US;
    while ((uint32_t)(fm1_ticks() - t0) < span)
        FM1_INPUT_IDLE();
}

static void fm1_input_init(void)
{
    static const uint8_t LEDP[4][2] = {{FM1_PH, 6}, {FM1_PH, 9}, {FM1_PA, 9}, {FM1_PA, 10}};
    const uint32_t rows_a = (1u << 0) | (1u << 5) | (1u << 6) | (1u << 7) | (1u << 8);
    const uint32_t sr = (1u << 1) | (1u << 3) | (1u << 4), row_b = 1u << 7;
    uint32_t i;
    for (i = 0; i < 4u; i++) {
        uint32_t p = LEDP[i][0], m = 1u << LEDP[i][1];
        FM1_PR(p, FM1_DIE) |= m;
        FM1_PR(p, FM1_OUT) &= ~m;
        FM1_PR(p, FM1_DIR) &= ~m;
        FM1_PR(p, FM1_HD0) |= m;
        FM1_PR(p, FM1_HD) |= m;
    }
    FM1_PR(FM1_PA, FM1_DIE) |= rows_a;
    FM1_PR(FM1_PA, FM1_DIR) |= rows_a;
    FM1_PR(FM1_PA, FM1_PD) &= ~rows_a;
    FM1_PR(FM1_PA, FM1_PU) |= rows_a;
    FM1_PR(FM1_PB, FM1_DIE) |= row_b;
    FM1_PR(FM1_PB, FM1_DIR) |= row_b;
    FM1_PR(FM1_PB, FM1_PD) &= ~row_b;
    FM1_PR(FM1_PB, FM1_PU) |= row_b;
    FM1_PR(FM1_PA, FM1_DIE) |= sr;
    FM1_PR(FM1_PA, FM1_PU) &= ~sr;
    FM1_PR(FM1_PA, FM1_PD) &= ~sr;
    FM1_PR(FM1_PA, FM1_OUT) &= ~sr;
    FM1_PR(FM1_PA, FM1_DIR) &= ~sr;
    fm1__sr_word(0xFFFFu);
    for (i = 0; i < FM1_NENC; i++)
        fm1_in.enc_prev[i] = fm1_in.enc_last[i] = 0xFF;   /* seeded by the first frame */
}

static void fm1__key(uint32_t id, uint32_t closed)
{
    volatile uint8_t *c = &fm1_in.cnt[id];
    uint32_t on;
    if (closed) {
        if (*c < FM1_DEBOUNCE)
            (*c)++;
    } else if (*c) {
        (*c)--;
    }
    if (*c == FM1_DEBOUNCE)
        on = 1;
    else if (*c == 0)
        on = 0;
    else
        return;
    if (id >= 14u) {
        if (on) {
            if (!(fm1_in.notes & (1u << (id - 14u))))
                fm1_in.notes_pressed |= 1u << (id - 14u);
            fm1_in.notes |= 1u << (id - 14u);
        }
        else
            fm1_in.notes &= ~(1u << (id - 14u));
    } else if (on != ((fm1_in.buttons >> id) & 1u)) {
        fm1_in.buttons ^= 1u << id;
        if (on)
            fm1_in.pressed |= 1u << id;
        else
            fm1_in.released |= 1u << id;
    }
}

static void fm1__frame(void);

static void fm1_input_scan(void)
{
    uint32_t p;
    for (p = 0; p < FM1_NCOL; p++) {
        fm1__led_lines(0);
        fm1__sr_word(0xFFFFu ^ (1u << p) ^ (p < 2u ? 1u << (11u + p) : 0u));
        fm1__wait(FM1_SETTLE_US);
        fm1_in.raw[p] = (uint8_t)fm1__rows();
        fm1__led_lines(fm1_led[p]);
        fm1__wait(FM1_LED_US);
    }
    fm1__led_lines(0);
    fm1__frame();
}

static void fm1__frame(void)
{
    uint32_t p, r, e;
    for (p = 0; p < FM1_NCOL; p++)
        for (r = 1; r < 5u; r++)
            if (FM1_KEYMAP[r][p] >= 0)
                fm1__key((uint32_t)FM1_KEYMAP[r][p], (fm1_in.raw[p] >> r) & 1u);
    for (e = 0; e < FM1_NENC; e++) {               /* quadrature decoder + detents */
        const uint8_t *m = FM1_ENC[e];
        uint32_t cur = ((fm1_in.raw[m[0]] >> m[1]) & 1u) << 1 | ((fm1_in.raw[m[2]] >> m[3]) & 1u);
        uint32_t idx;
        volatile int8_t *sub = &fm1_in.enc_sub[e];
        /* X0X: no two-scan filter. A state had to be seen on two scans running (2.2 ms) to count,
         * so a fast turn, whose states last less than that, did not move the knob at all. The
         * 1.1 ms between scans is longer than a contact bounce, and a step still needs >= 2 net
         * transitions between detents, so a glitch cannot make one. */
        if (cur != fm1_in.enc_last[e]) {
            fm1_in.enc_last[e] = (uint8_t)cur;
            fm1_in.enc_still[e] = 0;
        }
        if (fm1_in.enc_prev[e] == 0xFF) {          /* first frame: the knob rests here */
            fm1_in.enc_prev[e] = (uint8_t)cur;
            fm1_in.enc_rest[e] = (uint8_t)(1u << cur);
        }
        if (fm1_in.enc_still[e] < 255u && ++fm1_in.enc_still[e] == FM1_REST_FRAMES) {
            /* learn detent states: one state, or a complementary pair (00/11 or
             * 01/10). Anything else restarts the set; with 3-4 rest states every
             * arrival would look like a detent with |sub| < 2. */
            uint32_t r = fm1_in.enc_rest[e], bit = 1u << cur, comp = 1u << (cur ^ 3u);
            if (!(r & bit))
                fm1_in.enc_rest[e] = (uint8_t)(r == comp ? (r | bit) : bit);
        }
        if (cur == fm1_in.enc_prev[e])
            continue;
        idx = (uint32_t)fm1_in.enc_prev[e] << 2 | cur;
        if ((0x4182u >> idx) & 1u) {
            (*sub)++;
            fm1_in.enc_dir[e] = 1;
        } else if ((0x2814u >> idx) & 1u) {
            (*sub)--;
            fm1_in.enc_dir[e] = -1;
        } else {                                   /* both lines changed: a state skipped (a flick) */
            *sub = (int8_t)(*sub + 2 * fm1_in.enc_dir[e]);   /* two more the way it was going */
        }
        fm1_in.enc_prev[e] = (uint8_t)cur;
        if ((fm1_in.enc_rest[e] >> cur) & 1u) {    /* back on a detent */
            if (*sub >= 2)
                fm1_in.enc_steps[e]++;
            else if (*sub <= -2)
                fm1_in.enc_steps[e]--;
            *sub = 0;
        }
    }
    fm1_in.frames++;
}

/* the drive experiment: rowbit 3 = PA7, 4 = PA8, 2 = PA6 (the probe); strength 0..3 in HD0 / HD */
static void fm1__row_drive(uint32_t rowbit, uint32_t strength)
{
    uint32_t m = 1u << (rowbit == 3u ? 7 : rowbit == 4u ? 8 : 6);
    if (strength & 1u)
        FM1_PR(FM1_PA, FM1_HD0) |= m;
    else
        FM1_PR(FM1_PA, FM1_HD0) &= ~m;
    if (strength & 2u)
        FM1_PR(FM1_PA, FM1_HD) |= m;
    else
        FM1_PR(FM1_PA, FM1_HD) &= ~m;
    FM1_PR(FM1_PA, FM1_OUT) |= m;
    FM1_PR(FM1_PA, FM1_DIR) &= ~m;
}
static void fm1__row_release(uint32_t rowbit)
{
    uint32_t m = 1u << (rowbit == 3u ? 7 : rowbit == 4u ? 8 : 6);
    FM1_PR(FM1_PA, FM1_DIR) |= m;                  /* an input with its pull-up again */
    FM1_PR(FM1_PA, FM1_OUT) &= ~m;
    FM1_PR(FM1_PA, FM1_HD0) &= ~m;
    FM1_PR(FM1_PA, FM1_HD) &= ~m;
}
/* PA10 (the LED line of row PA6): an input with a pull-up for the probe, an output again after */
static void fm1__pa10_probe(int on)
{
    uint32_t m = 1u << 10;
    if (on) {
        FM1_PR(FM1_PA, FM1_OUT) &= ~m;
        FM1_PR(FM1_PA, FM1_PU) |= m;
        FM1_PR(FM1_PA, FM1_DIR) |= m;
    } else {
        FM1_PR(FM1_PA, FM1_PU) &= ~m;
        FM1_PR(FM1_PA, FM1_DIR) &= ~m;
    }
}
static void fm1__an_restore(void)
{
    if (fm1_an.driven) {
        fm1_an.driven = 0;
        fm1__row_release(fm1_an.rowbit);
    }
    if (fm1_an.probing) {
        fm1__row_release(fm1_an.rowbit);
        fm1__pa10_probe(0);
    }
}
/* the row probed in frame phase ph (>= 2): the (ph - 2)th set bit of fm1_an.probe, as a rowbit */
static uint32_t fm1__probe_row(uint32_t ph)
{
    uint32_t b, k = ph - 2u;
    for (b = 0; b < 3u; b++)
        if ((fm1_an.probe >> b) & 1u) {
            if (!k)
                return 2u + b;
            k--;
        }
    return 0;
}

/* one column per call, from a timer ISR (see top) */
static uint8_t fm1__tick_col;
static void fm1_input_tick(void)
{
    uint32_t p = fm1__tick_col, n = p + 1u == FM1_NCOL ? 0u : p + 1u;
    if (fm1_an.pend) {                             /* the conversion started last tick, on column p */
        uint32_t driven = fm1_an.driven, probing = fm1_an.probing;
        fm1_an.pend = 0;
        if (fm1_an.rowbit && fm1_an.selftest && !probing)
            FM1_PR(FM1_PA, FM1_DIR) |= 1u << (fm1_an.rowbit == 3u ? 7 : 8);
        fm1__an_restore();
        fm1_an.probing = 0;
        if (fm1_adc_done()) {
            uint32_t v = fm1_adc_take();
            if (probing)
                FM1_INPUT_PROBE(fm1_an.col, fm1_an.rowbit, v, (fm1_in.raw[fm1_an.col] >> fm1_an.rowbit) & 1u);
            else if (fm1_an.rowbit)
                FM1_INPUT_ANALOG(fm1_an.col, fm1_an.rowbit, v, driven);
            else
                fm1_an.master = (uint16_t)v;
        } else {
            fm1_adc_abort();
            fm1_an.timeouts++;
        }
    }
    fm1__led_lines(0);
    fm1_in.raw[p] = (uint8_t)fm1__rows();          /* column p has been latched one tick */
    fm1__sr_word(0xFFFFu ^ (1u << n) ^ (n < 2u ? 1u << (11u + n) : 0u));
    if (n == 0u) {
        uint32_t np = (fm1_an.probe & 1u) + ((fm1_an.probe >> 1) & 1u) + ((fm1_an.probe >> 2) & 1u);
        if (++fm1__dim_frame >= fm1_led_dim_div)
            fm1__dim_frame = 0;
        fm1__frame();
        fm1_an.phase = (uint8_t)(fm1_an.phase + 1u >= 2u + np ? 0u : fm1_an.phase + 1u);
    }
    if (fm1_an.con && fm1_an.phase >= 2u) {        /* a probe frame: the row of this phase, through PA10 */
        uint32_t pr = fm1__probe_row(fm1_an.phase);
        if (!pr || (pr == 2u ? n >= FM1_PROBE_COLS : FM1_KEYMAP[pr][n] < 14)) {
            fm1__led_lines(fm1__leds(n));          /* a button's column, or none: nothing to probe */
            fm1__tick_col = (uint8_t)n;
            return;
        }
        fm1__led_lines(fm1__leds(n) & ~(1u << 2));
        fm1__pa10_probe(1);
        fm1__row_drive(pr, 0u);                    /* the weakest, whatever DRIVE is: TOP LOW / HIGH were measured so */
        fm1__tick_col = (uint8_t)n;
        fm1_an.col = (uint8_t)n;
        fm1_an.rowbit = (uint8_t)pr;
        fm1_an.probing = 1;
        fm1_adc_start(FM1_ADC_LED_PA10, fm1_an.con);
        fm1_an.pend = 1;
        return;
    }
    fm1__led_lines(fm1__leds(n));
    fm1__tick_col = (uint8_t)n;
    if (fm1_an.con && fm1_an.phase < 2u) {         /* column n is latched now: sample it until the next tick */
        uint32_t odd = fm1_an.phase, ch, rowbit;
        if (odd && n == 0u) {
            ch = FM1_ADC_MASTER;
            rowbit = 0;
        } else {
            ch = odd ? FM1_ADC_ROW_PA8 : FM1_ADC_ROW_PA7;
            rowbit = odd ? 4u : 3u;
        }
        fm1_an.col = (uint8_t)n;
        fm1_an.rowbit = (uint8_t)rowbit;
        if (rowbit && !FM1_INPUT_LED_OK(n, rowbit))
            fm1__led_lines(0);                     /* a held key: no LED current in its column while it is read */
        if (rowbit && fm1_an.drive && !fm1_an.selftest && FM1_INPUT_DRIVE(n, rowbit)) {
            fm1__row_drive(rowbit, fm1_an.drive - 1u);
            fm1_an.driven = 1;
        }
        if (rowbit && fm1_an.selftest) {           /* the row driven low: the converter must see 0 */
            uint32_t m = 1u << (rowbit == 3u ? 7 : 8);
            FM1_PR(FM1_PA, FM1_OUT) &= ~m;
            FM1_PR(FM1_PA, FM1_DIR) &= ~m;
        }
        if ((!rowbit && fm1_an.time_req) || fm1_an.immediate) {
            uint32_t t0 = fm1_ticks();
            fm1_adc_start(ch, fm1_an.con);
            while (!fm1_adc_done() && fm1_ticks() - t0 < 95u * FM1_TICKS_PER_US)
                ;
            if (!rowbit && fm1_an.time_req) {      /* once, for the screen: how long a conversion takes */
                fm1_an.time_req = 0;
                fm1_an.us = (uint16_t)((fm1_ticks() - t0) / FM1_TICKS_PER_US);
            }
            if (fm1_an.immediate) {
                if (fm1_adc_done()) {
                    uint32_t v = fm1_adc_take();
                    if (rowbit)
                        FM1_INPUT_ANALOG(n, rowbit, v, fm1_an.driven);
                    else
                        fm1_an.master = (uint16_t)v;
                } else {
                    fm1_adc_abort();
                    fm1_an.timeouts++;
                }
            } else {
                fm1_an.pend = 1;
            }
        } else {
            fm1_adc_start(ch, fm1_an.con);
            fm1_an.pend = 1;
        }
        if (rowbit && fm1_an.selftest) {
            uint32_t m = 1u << (rowbit == 3u ? 7 : 8);
            if (fm1_an.immediate)                  /* else the row stays low until the result is taken */
                FM1_PR(FM1_PA, FM1_DIR) |= m;
        }
        if (fm1_an.immediate && fm1_an.driven)    /* else the row stays driven until the result is taken */
            fm1__an_restore();
    }
}

/* main-loop critical section against fm1_input_tick (main loop only: it
 * re-enables interrupts unconditionally) */
static inline uint32_t fm1__lock(void)
{
    __asm__ volatile("cli" ::: "memory");
    return 0;
}
static inline void fm1__unlock(uint32_t v)
{
    (void)v;
    __asm__ volatile("csync\n\tsti" ::: "memory");
}

/* detent steps turned since the last call, + = clockwise */
static int32_t fm1_enc_take(uint32_t e)
{
    uint32_t k = fm1__lock();
    int32_t s = fm1_in.enc_steps[e];
    fm1_in.enc_steps[e] = 0;
    fm1__unlock(k);
    return s;
}

static uint32_t fm1_input_edges(uint32_t *released)
{
    uint32_t k = fm1__lock();
    uint32_t p = fm1_in.pressed;
    if (released)
        *released = fm1_in.released;
    fm1_in.pressed = fm1_in.released = 0;
    fm1__unlock(k);
    return p;
}

static uint32_t fm1_input_note_edges(void)
{
    uint32_t k = fm1__lock();
    uint32_t p = fm1_in.notes_pressed;
    fm1_in.notes_pressed = 0;
    fm1__unlock(k);
    return p;
}

/* LED of key id (button 0..13 or note key 14..40) */
static void fm1_led_key(uint32_t id, int on)
{
    uint32_t p, r;
    for (p = 0; p < FM1_NCOL; p++)
        for (r = 1; r < 5u; r++)
            if (FM1_KEYMAP[r][p] == (int8_t)id) {
                if (on)
                    fm1_led[p] |= (uint8_t)(1u << r);
                else
                    fm1_led[p] &= (uint8_t)~(1u << r);
            }
}

/* Capacitance probe (fm1-velocity experiment): drive a matrix row low, release it to its
 * pull-up, and count loop iterations until the input reads high. The time is the row's
 * capacitance against the pull-up; a finger over a key cap on that row adds to it. IRQs must be
 * off (the scan tick would read the row low as keys). A row held low by a pressed key never
 * rises: the count saturates at FM1_CAP_MAX. rowbit 3 = PA7, 4 = PA8. */
#define FM1_CAP_MAX 4000u
static uint32_t fm1_cap_probe(uint32_t rowbit)
{
    uint32_t m = 1u << (rowbit == 3u ? 7 : 8), n = 0, i;
    FM1_PR(FM1_PA, FM1_OUT) &= ~m;
    FM1_PR(FM1_PA, FM1_DIR) &= ~m;                 /* output low: discharge */
    for (i = 0; i < 40u; i++)
        __asm__ volatile("" ::: "memory");
    FM1_PR(FM1_PA, FM1_DIR) |= m;                  /* input again, the pull-up charges it */
    while (!(FM1_PR(FM1_PA, FM1_IN) & m) && n < FM1_CAP_MAX)
        n++;
    return n;
}
