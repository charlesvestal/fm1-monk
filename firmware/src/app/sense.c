/* SPDX-License-Identifier: GPL-3.0-only */
/* Velocity sensing from the key's contact resistance.
 *
 * Each key on the two analog rows is sampled every ~2.2 ms (fm1_input.h): the ADC reading of
 * the row against its pull-up, 1023 with the key open, falling as the carbon pill under the key
 * is pressed harder (its resistance drops with force). A press crosses TOUCH (the pill has
 * started to conduct); the dome snaps, so the pill lands within a sample or two whatever the
 * speed, and what tells a hard hit from a soft one is how deep the reading is just after the
 * landing (measured on the FM-1: ~185 for a hard hit, 270..360 for a gentle press, one sample
 * after the touch). So the velocity comes from the mean of the `wait` samples after the touch
 * sample (the touch sample itself depends on where in the fall it lands: a regular press read
 * as soft when it caught the fall halfway), once the key is below PRESS (seated), and
 * relative to the key's floor, the lowest reading it has ever seated at (keys differ by tens of
 * counts): v_lo or less above the floor is 127, v_hi or more is 1, linear between. A key that
 * sinks below PRESS only later is the slowest kind; one that never gets there is a graze.
 * Held: above PRESS it is lifting, two samples running above RELEASE and it is off (a landed key
 * can read open for a single sample: a bounce, which retriggered), and the time from leaving
 * PRESS to crossing RELEASE is the release velocity.
 *
 * Aftertouch: a held key keeps reporting. The pill saturates when pressed firmly (the floor), so
 * the sensitive range is in easing off: sense_pressure() is 0 at the floor and 127 at_range
 * above it, polyphonic, the reading's noise being a count or two. A key relaxing moves the
 * reading by a few counts up to a few tens; the lift (above PRESS) is not pressure.
 *
 * Drive (sn_cfg.drive): with the matrix diode in the divider, the pull-up sees a seated key as
 * the diode's ~0.6 V (the floor of ~185) and a harder press only 10..20 counts lower. With drive
 * on, a key that has been touched is sampled every other time with its row driven high
 * (fm1_input.h: the pin's driver, tens to hundreds of ohms, becomes the upper resistor), which
 * spreads the seated range. The state machine still runs on the pulled-up samples (a driven key
 * that grazes reads open as well, so only those can tell a touch from a seat); the driven ones
 * (k->d) give the velocity and the aftertouch. The velocity is the lowest driven reading since the
 * touch (measured: a sample that catches the key still falling reads high, 587 for a hit that then
 * sat at 407, so a mean would make it soft), on a log scale: with the weak driver the pad reads the
 * diode (SN_DBASE) plus a fixed current through the pill, so reading - SN_DBASE is the pill's
 * resistance, and conductive rubber's resistance falls as a power of the force: its log is linear
 * in the log of the force. t = log(r / r_floor) / log(r_ceil / r_floor), 0 at the key's driven
 * floor (its hardest), 1 at its ceiling (its lightest press that seats); velocity = 1 + 126 (1 - t)^curve
 * (d_curve / 100). Measured on 830 presses: curve 0.7 puts regular playing at 45 / 63 / 80
 * (quartiles), with 127 and 1 both rare; the square curve on the raw reading before it had a
 * median of 32. The aftertouch is pressing harder than the note landed: 0 at the landing reading,
 * 127 d_at_range below it (in pull mode it is easing off from the floor, the only range that
 * reading has; driven, that made every lightly held note full pressure). With WAIT 2 the velocity
 * waits for two driven samples (6.6 ms after the touch instead of 4.4).
 *
 * Calibration (sense_learn): each key's floor and ceiling follow its own landings, as running
 * quantiles: the floor near its firmest 1/32 of presses, the ceiling near its lightest 1/32. A
 * landing beyond an end moves it by most of a step (64 counts for a key's first 16 presses, 24
 * after), one inside creeps it back by 1/32 of a step (1/8 for the first 16, so an end that starts
 * too wide comes in within them); so a freak press moves an end by at most a step, and the ends
 * settle on "your firm" and "your light", not the extremes. They start from the
 * level's default floor and LIGHTEST (d_ceil), and are kept at least SN_SPAN apart. Measured: keys
 * differ by ~120 at the firm end and ~250 at the light end, and the light end is where off-centre
 * presses live. sense_want_drive() tells the scan which sample
 * to drive: the first after the touch, then every other one while the key is held.
 *
 * The top row (sense_probe, per key: the other rows can be probed too, to compare): PA6's keys have no converter; fm1_input.h's probe reads their
 * column's low level through the LED line PA10 with PA6 driven high. A pressed key lifts it, more
 * when pressed harder (or so the experiment asks). Each key keeps a baseline (the reading while
 * the digital scan sees it open, smoothed) and the peak above it since the press;
 * sense_probe_velocity() maps the peak: t_lo above the baseline = 1, t_hi = 127, linear between.
 * Measured (DRIVE 1's strength on PA6): any contact lifts it ~20, a light hold ~25..34, a squeeze
 * 40..46; mapped from zero, the softest press was already ~50.
 *
 * sense_sample() runs in the scan interrupt. It also records a capture of the latest press (8
 * samples before the touch, SN_CAP in all) for the screen and the host log, and queues events
 * for the main loop (sense_take). No HAL here: tests/host/sense_test.c runs it with synthetic
 * curves. */
#include <stdint.h>
#include "../dsp/fastmath.h"

#define SN_NKEYS 27
#define SN_PRE 8
#define SN_CAP 48                      /* samples of a capture (~105 ms at 2.2 ms) */
#define SN_NEV 16

enum { SN_OPEN, SN_TOUCH, SN_DOWN, SN_LIFT };
enum { EV_NONE, EV_ON, EV_OFF, EV_GRAZE };     /* GRAZE: touched, never pressed */

typedef struct {
    uint16_t touch, press, release;    /* ADC thresholds (1023 = open) */
    uint8_t wait;                      /* samples after the touch over which the velocity is read (0 = the touch sample) */
    uint16_t v_lo, v_hi;               /* above the key's floor: the mean that means vmax, and the one that means 1 */
    uint8_t vmax;                      /* the velocity of a played key (at the floor) */
    uint16_t at_range;                 /* aftertouch: the reading above the floor that means 127 */
    uint16_t ticks_per_ms;
    uint8_t drive;                     /* velocity and aftertouch from the driven samples */
    uint16_t d_curve;                  /* driven velocity: the curve's exponent x 100 */
    uint16_t d_ceil;                   /* driven velocity: the reading of the lightest press that seats (= 1) */
    uint16_t d_at_range;               /* driven aftertouch: the drop below the landing that means 127 */
    uint16_t t_lo, t_hi;               /* the top row: the probe's rise above its baseline that means 1, and 127 */
    uint8_t learn;                     /* MONK-1: the calibration (sense_learn) on; off by default, as Velo's first
                                        * session with it played more extremes, not fewer (Velo notes/01-lab.md) */
} sense_cfg_t;

typedef struct {
    uint16_t v, vmin;
    uint8_t st;
    uint8_t vel;                       /* the last note's velocity */
    uint8_t since;                     /* samples since the touch */
    uint8_t up;                        /* samples running above RELEASE while lifting */
    uint16_t floor_;                   /* the lowest reading the key has seated at (learned) */
    uint32_t acc;                      /* the readings since the touch, summed */
    uint32_t t_touch, t_lift;
    uint16_t pre[SN_PRE];              /* the samples before the touch, a ring */
    uint8_t pi;
    uint8_t dnext;                     /* drive the next sample */
    uint16_t d, dmin, dfloor;          /* driven: the last reading, the lowest this press, the key's floor */
    uint16_t dceil;                    /* driven: the key's ceiling (its lightest landing: velocity 1) */
    uint32_t f16, c16;                 /* the floor and ceiling x 16 (the learning's resolution) */
    uint8_t nland;                     /* landings learned from (to 255) */
    uint16_t dland;                    /* driven: the reading the velocity came from (the aftertouch's zero) */
    uint32_t dacc;                     /* driven readings since the touch, summed */
    uint8_t dn;                        /* and how many */
} sense_key_t;

typedef struct {
    uint16_t v;                        /* the last probe reading */
    uint16_t base;                     /* the reading while open, x 1 (smoothed in base16) */
    uint16_t base16;
    uint16_t peak;                     /* the highest reading above base since the press */
    uint8_t down;
    uint8_t n;                         /* readings since the press */
} sense_probe_t;

#define SN_DRIVEN 0x400u                /* a capture sample with this bit was a driven reading */
typedef struct {
    uint8_t key, n, vel, done;
    uint16_t vdec;                     /* the reading the velocity was taken from (| SN_DRIVEN: a driven mean) */
    uint16_t v[SN_CAP];
} sense_cap_t;

typedef struct {
    uint8_t type, key, vel;
    uint8_t dt_ms;                     /* on: touch to the velocity sample; off: lift to release, in ms */
} sense_ev_t;

static sense_cfg_t sn_cfg = {900, 400, 600, 1, 0, 72, 127, 32, 24000, 0, 70, 1000, 64, 20, 48, 0};
#define SN_FLOOR0 190u                 /* a key's floor before it has been played hard */
/* the driven floor before a key has seated lower, by DRIVE level: a little above the firm presses
 * measured on key 3 (389 / 547 / 740 / 805 at levels 1..4; keys differ by up to ~120). The floor
 * only learns downward, so a default below a key's true floor would keep its hardest hit from 127
 * (450 at DRIVE 2: ~95). A first press taken as the floor made a soft one 127. */
static const uint16_t SN_DFLOOR0[5] = {450, 450, 600, 790, 860};
#define SN_DFLOOR_DEF (SN_DFLOOR0[sn_cfg.drive > 4u ? 4u : sn_cfg.drive])
#define SN_SPAN 80u                    /* the least a key's ceiling sits above its floor */
static volatile uint8_t sn_cal_changed; /* a key's floor or ceiling moved since the main loop last saved */
#define SN_DBASE 185u                  /* the driven reading with no pill resistance: the diode and the 595 */
#define SN_TOP0 20u                    /* the first top key (notes 20..26: row PA6, columns 0..6) */
#define SN_NTOP 7u
static sense_key_t sn_key[SN_NKEYS];
static sense_probe_t sn_probe[SN_NKEYS];    /* the PA10 probe, per key (the top row; the others to compare) */
static uint32_t sn_analog;             /* bit k: key k is on an analog row */
static sense_cap_t sn_cap_w, sn_cap;   /* being written; the last complete one */
static volatile uint8_t sn_cap_seq;    /* bumps when sn_cap is replaced */
static sense_ev_t sn_ev[SN_NEV];
static volatile uint8_t sn_ev_w, sn_ev_r;
static uint32_t sn_samples;

static void sense_cal_reset(void);
static void sense_init(uint32_t analog_mask)
{
    uint32_t k;
    sn_analog = analog_mask;
    for (k = 0; k < SN_NKEYS; k++) {
        sn_key[k].v = sn_key[k].vmin = 1023;
        sn_key[k].floor_ = SN_FLOOR0;
        sn_key[k].st = SN_OPEN;
    }
    sense_cal_reset();
}

static void sn_push(uint8_t type, uint8_t key, uint8_t vel, uint32_t dt_ms)
{
    if ((uint8_t)(sn_ev_w - sn_ev_r) < SN_NEV) {
        sense_ev_t *e = &sn_ev[sn_ev_w % SN_NEV];
        e->type = type;
        e->key = key;
        e->vel = vel;
        e->dt_ms = (uint8_t)(dt_ms > 255u ? 255u : dt_ms);
        sn_ev_w++;
    }
}

/* velocity from the landing reading above the key's floor: v_lo -> vmax, v_hi -> 1, linear between */
static uint8_t sense_velocity(uint32_t rel)
{
    uint32_t v = rel, lo = sn_cfg.v_lo, hi = sn_cfg.v_hi, top = sn_cfg.vmax ? sn_cfg.vmax : 1u;
    if (hi <= lo)
        hi = lo + 1u;
    if (v <= lo)
        return (uint8_t)top;
    if (v >= hi)
        return 1;
    return (uint8_t)(1u + ((top - 1u) * (hi - v) + (hi - lo) / 2u) / (hi - lo));
}

/* the velocity of driven reading d on a key whose floor is fl and ceiling cl: the log scale (top) */
static uint8_t sense_velocity_d(uint32_t d, uint32_t fl, uint32_t cl)
{
    float top = (float)(sn_cfg.vmax ? sn_cfg.vmax : 1u), r, r0, r1, t, c;
    if (d <= fl)
        return (uint8_t)top;
    if (d >= cl || cl <= fl + 1u)
        return 1;
    r = (float)(d > SN_DBASE ? d - SN_DBASE : 1u);
    r0 = (float)(fl > SN_DBASE ? fl - SN_DBASE : 1u);
    r1 = (float)(cl - SN_DBASE);
    t = fm_log2f(r / r0) / fm_log2f(r1 / r0);
    t = fm_clampf(t, 0.0f, 1.0f);
    c = (float)(sn_cfg.d_curve ? sn_cfg.d_curve : 1u) * 0.01f;
    return (uint8_t)(1.0f + (top - 1.0f) * fm_powf(1.0f - t, c) + 0.5f);
}

/* aftertouch of a held key: 0 at its floor (pressed firmly), 127 at_range above it (eased off) */
static uint8_t sense_pressure(uint32_t key)
{
    const sense_key_t *k;
    uint32_t rel, range;
    if (key >= SN_NKEYS || sn_key[key].st != SN_DOWN)   /* lifting is not easing off */
        return 0;
    k = &sn_key[key];
    if (sn_cfg.drive && k->dn) {                   /* pressing harder than the landing */
        range = sn_cfg.d_at_range ? sn_cfg.d_at_range : 1u;
        rel = k->dland > k->d ? (uint32_t)(k->dland - k->d) : 0u;
    } else {
        range = sn_cfg.at_range ? sn_cfg.at_range : 1u;
        rel = k->v > k->floor_ ? (uint32_t)(k->v - k->floor_) : 0u;
    }
    if (rel >= range)
        return 127;
    return (uint8_t)((127u * rel + range / 2u) / range);
}

static void sn_cap_add(uint16_t v)
{
    if (sn_cap_w.n < SN_CAP)
        sn_cap_w.v[sn_cap_w.n++] = v;
    if (sn_cap_w.n == SN_CAP && !sn_cap_w.done) {
        sn_cap_w.done = 1;
        sn_cap = sn_cap_w;
        sn_cap_seq++;
    }
}

/* one key's ends from x16 values, kept apart and in range */
static void sn_cal_put(sense_key_t *k, uint32_t f16, uint32_t c16)
{
    if (f16 < (SN_DBASE + 16u) << 4)
        f16 = (SN_DBASE + 16u) << 4;
    if (c16 > 1020u << 4)
        c16 = 1020u << 4;
    if (c16 < f16 + (SN_SPAN << 4)) {              /* too close: part them about the middle */
        uint32_t mid = (f16 + c16) / 2u;
        f16 = mid - (SN_SPAN << 3);
        c16 = mid + (SN_SPAN << 3);
    }
    k->f16 = f16;
    k->c16 = c16;
    k->dfloor = (uint16_t)(f16 >> 4);
    k->dceil = (uint16_t)(c16 >> 4);
}

/* the calibration back to the defaults: the level's floor, LIGHTEST (it belongs to one strength) */
static void sense_cal_reset(void)
{
    uint32_t k;
    for (k = 0; k < SN_NKEYS; k++) {
        sn_cal_put(&sn_key[k], (uint32_t)SN_DFLOOR_DEF << 4, (uint32_t)sn_cfg.d_ceil << 4);
        sn_key[k].nland = 0;
    }
    sn_cal_changed = 1;
}
static void sense_forget_dfloor(void) { sense_cal_reset(); }

/* every key's ceiling to LIGHTEST (the knob turned: start the light end over) */
static void sense_cal_ceil_all(void)
{
    uint32_t k;
    for (k = 0; k < SN_NKEYS; k++)
        sn_cal_put(&sn_key[k], sn_key[k].f16, (uint32_t)sn_cfg.d_ceil << 4);
    sn_cal_changed = 1;
}

/* a landing of key k (the driven reading its velocity came from): the running quantiles (top) */
static void sense_learn(sense_key_t *k, uint32_t m)
{
    uint32_t early = k->nland < 16u, step = early ? 64u << 4 : 24u << 4, f = k->f16, c = k->c16, m16 = m << 4;
    uint32_t creep = early ? step / 8u : step / 32u, jump = step - creep;
    if (!sn_cfg.learn)
        return;
    if (m16 < f)
        f -= (f - m16 < jump ? f - m16 : jump);
    else
        f += creep;
    if (m16 > c)
        c += (m16 - c < jump ? m16 - c : jump);
    else
        c -= creep;
    sn_cal_put(k, f, c);
    if (k->nland < 255u)
        k->nland++;
    sn_cal_changed = 1;
}

/* the main loop's copy of the calibration (to save), and back (restored at power-on) */
static void sense_cal_get(uint16_t *fl, uint16_t *cl, uint8_t *n)
{
    uint32_t k;
    for (k = 0; k < SN_NKEYS; k++) {
        fl[k] = sn_key[k].dfloor;
        cl[k] = sn_key[k].dceil;
        n[k] = sn_key[k].nland;
    }
}
static void sense_cal_set(const uint16_t *fl, const uint16_t *cl, const uint8_t *n)
{
    uint32_t k;
    for (k = 0; k < SN_NKEYS; k++) {
        sn_cal_put(&sn_key[k], (uint32_t)fl[k] << 4, (uint32_t)cl[k] << 4);
        sn_key[k].nland = n[k];
    }
}

/* the scan asks before each sample: drive it? */
static int sense_want_drive(uint32_t key) { return key < SN_NKEYS && sn_cfg.drive && sn_key[key].dnext; }

/* a driven sample: no state change (a driven graze reads open too), only the force reading */
static void sn_driven(sense_key_t *k, uint32_t key, uint32_t v)
{
    k->dnext = 0;
    if (k->st == SN_OPEN)                          /* released meanwhile */
        return;
    k->d = (uint16_t)v;
    if (sn_cap_w.key == key && !sn_cap_w.done)
        sn_cap_add((uint16_t)(v | SN_DRIVEN));
    if (k->st == SN_TOUCH) {
        k->dacc += v;
        k->dn++;
    }
    if (k->st != SN_LIFT && v < k->dmin)
        k->dmin = (uint16_t)v;
}

static void sense_sample(uint32_t key, uint32_t v, uint32_t now, int driven)
{
    sense_key_t *k;
    if (key >= SN_NKEYS)
        return;
    k = &sn_key[key];
    sn_samples++;
    if (driven) {
        sn_driven(k, key, v);
        return;
    }
    k->v = (uint16_t)v;
    if (sn_cap_w.key == key && !sn_cap_w.done)
        sn_cap_add((uint16_t)v);
    /* the states fall through within one sample: a key that goes from open to seated between
     * two samples is a press with no time between touch and press, the fastest there is */
    if (k->st == SN_OPEN && v < sn_cfg.touch) {
        uint32_t i;
        k->st = SN_TOUCH;
        k->t_touch = now;
        k->since = 0;
        k->acc = 0;                        /* the touch sample is left out of the mean */
        k->vmin = (uint16_t)v;
        k->dacc = 0;
        k->dn = 0;
        k->dmin = 1023;
        /* a new capture: the 8 samples before, then this one */
        sn_cap_w.key = (uint8_t)key;
        sn_cap_w.n = 0;
        sn_cap_w.done = 0;
        sn_cap_w.vel = 0;
        sn_cap_w.vdec = 0;
        for (i = 0; i < SN_PRE; i++)
            sn_cap_w.v[sn_cap_w.n++] = k->pre[(k->pi + i) % SN_PRE];
        sn_cap_add((uint16_t)v);
    }
    if (k->st == SN_TOUCH) {
        if (v < k->vmin)
            k->vmin = (uint16_t)v;
        if (k->since)
            k->acc += v;
        if (v >= sn_cfg.touch) {                   /* gone before the velocity sample: a graze */
            k->st = SN_OPEN;
            sn_push(EV_GRAZE, (uint8_t)key, 0, (now - k->t_touch) / sn_cfg.ticks_per_ms);
        } else if (k->since >= sn_cfg.wait && v < sn_cfg.press) {   /* seated: the velocity from the readings since */
            uint32_t dt = now - k->t_touch, m = k->since ? k->acc / k->since : v;
            k->st = SN_DOWN;
            if (sn_cfg.drive && k->dn) {           /* the lowest driven reading since the touch */
                m = k->dmin;
                k->dland = (uint16_t)m;
                k->vel = sense_velocity_d(m, k->dfloor, k->dceil);
                sense_learn(k, m);
                m |= SN_DRIVEN;
            } else {
                k->vel = sense_velocity(m > k->floor_ ? m - k->floor_ : 0u);
            }
            if (sn_cap_w.key == key) {
                sn_cap_w.vel = k->vel;
                sn_cap_w.vdec = (uint16_t)m;
            }
            sn_push(EV_ON, (uint8_t)key, k->vel, dt / sn_cfg.ticks_per_ms);
        } else if (k->since < 255u) {
            k->since++;
        }
    } else if (k->st == SN_DOWN) {
        if (v < k->vmin)
            k->vmin = (uint16_t)v;
        if (v >= sn_cfg.press) {
            k->st = SN_LIFT;
            k->t_lift = now;
            k->up = 0;
        }
    }
    if (k->st == SN_LIFT) {
        if (v < sn_cfg.press) {
            k->st = SN_DOWN;
        } else if (v < sn_cfg.release) {
            k->up = 0;
        } else if (++k->up >= 2u) {
            k->st = SN_OPEN;
            if (k->vmin < k->floor_)              /* the key seated lower than ever: its floor */
                k->floor_ = k->vmin;
            sn_push(EV_OFF, (uint8_t)key, k->vel, (now - k->t_lift) / sn_cfg.ticks_per_ms);
        }
    }
    k->pre[k->pi] = (uint16_t)v;
    k->pi = (uint8_t)((k->pi + 1u) % SN_PRE);
    k->dnext = (uint8_t)(sn_cfg.drive && k->st != SN_OPEN);   /* a held key: the next one driven */
}

/* a probe reading of key k, with the digital scan's view of it */
static void sense_probe(uint32_t k, uint32_t v, uint32_t pressed)
{
    sense_probe_t *p;
    if (k >= SN_NKEYS)
        return;
    p = &sn_probe[k];
    p->v = (uint16_t)v;
    if (!p->base16)
        p->base16 = (uint16_t)(v << 4);
    if (!pressed) {
        p->down = 0;
        p->peak = 0;
        p->n = 0;
        p->base16 = (uint16_t)(p->base16 + (((int32_t)v << 4) - (int32_t)p->base16) / 8);
        p->base = (uint16_t)(p->base16 >> 4);
    } else {
        uint32_t rel = v > p->base ? v - p->base : 0u;
        p->down = 1;
        if (p->n < 255u)
            p->n++;
        if (rel > p->peak)
            p->peak = (uint16_t)rel;
    }
}

/* the velocity of key k from the probe's peak since the press, 0 when there is no reading */
static uint8_t sense_probe_velocity(uint32_t t)
{
    uint32_t lo = sn_cfg.t_lo, hi = sn_cfg.t_hi > sn_cfg.t_lo ? sn_cfg.t_hi : sn_cfg.t_lo + 1u, pk;
    if (t >= SN_NKEYS || !sn_probe[t].down)
        return 0;
    pk = sn_probe[t].peak;
    if (pk <= lo)
        return 1;
    if (pk >= hi)
        return 127;
    return (uint8_t)(1u + (126u * (pk - lo) + (hi - lo) / 2u) / (hi - lo));
}

/* main loop: the next event, 0 when none */
static int sense_take(sense_ev_t *e)
{
    if (sn_ev_r == sn_ev_w)
        return 0;
    *e = sn_ev[sn_ev_r % SN_NEV];
    sn_ev_r++;
    return 1;
}

static int sense_is_analog(uint32_t key) { return (sn_analog >> key) & 1u; }
