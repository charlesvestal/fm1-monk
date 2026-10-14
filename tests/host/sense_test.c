/* SPDX-License-Identifier: GPL-3.0-only */
/* Velo's sensing state machine on curves like the ones measured on the FM-1: a hard hit lands
 * near 185 at once, a gentle press sits at 300+ first; a graze, a bounce while held, the capture,
 * and the velocity map. Samples 2.2 ms apart, as the scan delivers them. */
#include <stdio.h>
#include <stdlib.h>
#include "../../firmware/src/app/sense.c"

#define SAMPLE_TICKS 52800u            /* 2.2 ms */
static uint32_t now;
static int fails;

#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void feed(uint32_t key, uint32_t v) { sense_sample(key, v, now, 0); now += SAMPLE_TICKS; }
/* as the scan does it with DRIVE on: the sample is driven when sense asks for it, and then reads
 * `driven` instead of the pulled-up `pulled` */
static void feed2(uint32_t key, uint32_t pulled, uint32_t driven)
{
    int d = sense_want_drive(key);
    sense_sample(key, d ? driven : pulled, now, d);
    now += SAMPLE_TICKS;
}
static int take(sense_ev_t *e) { return sense_take(e); }
static void drain(void) { sense_ev_t e; while (take(&e)) ; }
static void settle(uint32_t key, int n) { while (n--) feed(key, 1023); }

static void press_in(uint32_t key, uint32_t first, uint32_t second, uint32_t bottom)   /* the samples after the touch, then hold */
{
    int i;
    feed(key, first);
    feed(key, second);
    for (i = 0; i < 4; i++)
        feed(key, bottom);
}

int main(void)
{
    sense_ev_t e;
    uint8_t v_fast, v_mid, v_slow;
    sense_init(0xFFFFFu);
    sn_cfg.learn = 1;                          /* the calibration is tested too (MONK-1 runs with it off) */
    now = 1000;
    settle(3, 20);

    /* a hard hit: seated by the sample after the touch (the mean lands ~14 above the floor) */
    press_in(3, 224, 185, 185);
    CHECK(take(&e) && e.type == EV_ON && e.key == 3);
    v_fast = e.vel;
    CHECK(v_fast >= 100);
    /* a release: up through PRESS and RELEASE, two samples above RELEASE */
    feed(3, 500);
    feed(3, 700);
    CHECK(!take(&e));
    feed(3, 700);
    CHECK(take(&e) && e.type == EV_OFF && e.key == 3 && e.vel == v_fast);
    CHECK(!take(&e));
    settle(3, 20);

    /* a normal press (measured: lands ~15 above the floor) */
    press_in(3, 215, 192, 186);
    CHECK(take(&e) && e.type == EV_ON);
    v_mid = e.vel;
    settle(3, 20);
    drain();

    /* a gentle press (measured: 300 then 250, sinking over 20 ms) */
    press_in(3, 300, 250, 200);
    CHECK(take(&e) && e.type == EV_ON);
    v_slow = e.vel;
    settle(3, 20);
    drain();
    printf("  hard %u  normal %u  gentle %u\n", v_fast, v_mid, v_slow);
    CHECK(v_fast > v_mid && v_mid > v_slow);
    CHECK(v_mid >= 90 && v_mid < 127 && v_slow >= 1 && v_slow < 30);

    /* a graze: touched, never seated */
    feed(5, 850);
    feed(5, 820);
    feed(5, 1023);
    CHECK(take(&e) && e.type == EV_GRAZE && e.key == 5);
    CHECK(!take(&e));

    /* a bounce while held: one sample above PRESS, or one reading open, does not release */
    press_in(7, 300, 100, 100);
    CHECK(take(&e) && e.type == EV_ON && e.key == 7);
    feed(7, 450);
    feed(7, 100);
    feed(7, 100);
    feed(7, 1023);
    feed(7, 100);
    feed(7, 100);
    CHECK(!take(&e));
    feed(7, 1023);
    feed(7, 1023);
    CHECK(take(&e) && e.type == EV_OFF);
    settle(7, 20);

    /* the capture: 8 samples before the touch, then the press; complete after SN_CAP samples */
    {
        uint8_t seq = sn_cap_seq;
        int i;
        settle(9, 10);
        press_in(9, 400, 150, 150);
        drain();
        for (i = 0; i < SN_CAP; i++)
            feed(9, 150);
        CHECK(sn_cap_seq == (uint8_t)(seq + 1));
        CHECK(sn_cap.key == 9 && sn_cap.n == SN_CAP);
        for (i = 0; i < SN_PRE; i++)
            CHECK(sn_cap.v[i] == 1023);
        CHECK(sn_cap.v[SN_PRE] < sn_cfg.touch);
        CHECK(sn_cap.vel > 0 && sn_cap.vdec == 150);
    }

    /* the map: 127 at v_lo and below, 1 at v_hi and above, monotonic between */
    {
        uint32_t v, last = 128;
        CHECK(sense_velocity(0) == 127);
        CHECK(sense_velocity(sn_cfg.v_lo) == 127);
        CHECK(sense_velocity(sn_cfg.v_hi) == 1);
        CHECK(sense_velocity(1023) == 1);
        for (v = 0; v <= 400; v += 10) {
            uint8_t x = sense_velocity(v);
            CHECK(x <= last);
            last = x;
        }
    }
    /* the floor: a key that seats at 170 once reads 127 at a mean of 185 from then on */
    {
        settle(2, 10);
        press_in(2, 200, 170, 170);
        drain();
        feed(2, 1023);
        feed(2, 1023);
        drain();
        CHECK(sn_key[2].floor_ == 170);
        press_in(2, 170, 170, 170);
        CHECK(take(&e) && e.type == EV_ON && e.vel == 127);
        settle(2, 10);
        drain();
        CHECK(sn_key[3].floor_ < SN_FLOOR0);     /* key 3 above learned its own from the hard hit */
    }

    /* aftertouch: 0 at the floor, rising as the key is eased off, 127 at_range above */
    {
        settle(4, 10);
        press_in(4, 200, 170, 170);
        drain();
        feed(4, 1023);
        feed(4, 1023);
        drain();
        press_in(4, 200, 170, 170);
        CHECK(sense_pressure(4) == 0);
        feed(4, 170 + 16);
        CHECK(sense_pressure(4) == 127 / 2 + 1 || sense_pressure(4) == 127 / 2);
        feed(4, 170 + 40);
        CHECK(sense_pressure(4) == 127);
        feed(4, 170 + 400);
        CHECK(sense_pressure(4) == 0);            /* above PRESS: lifting, not easing off */
        feed(4, 1023);
        feed(4, 1023);
        CHECK(sense_pressure(4) == 0);
        drain();
    }

    /* drive: a touched key's next sample is driven, then every other one while held; the velocity
     * comes from the driven readings above the driven floor, the state from the pulled-up ones */
    {
        uint8_t v_hard, v_soft;
        sn_cfg.drive = 1;
        sn_cfg.d_curve = 70;                       /* the measured scale */
        sn_cfg.d_ceil = 1000;
        sn_cfg.d_at_range = 64;
        sn_cfg.wait = 1;
        settle(11, 10);
        CHECK(!sense_want_drive(11));              /* open keys are never driven */
        feed2(11, 230, 999);                       /* the touch: pulled up */
        CHECK(sense_want_drive(11));
        feed2(11, 185, 420);                       /* driven: a hard hit seats deep, below the assumed floor */
        CHECK(!take(&e));                          /* the seat is a pulled-up sample's call */
        CHECK(!sense_want_drive(11));
        feed2(11, 185, 420);
        CHECK(take(&e) && e.type == EV_ON && e.key == 11);
        CHECK((sn_cap_w.vdec & SN_DRIVEN) && (sn_cap_w.vdec & 0x3FFu) == 420);
        v_hard = e.vel;
        CHECK(v_hard == 127);
        /* held: the aftertouch is pressing harder than the landing (420): easing off is none */
        feed2(11, 185, 500);
        feed2(11, 185, 500);
        CHECK(sense_pressure(11) == 0);
        feed2(11, 185, 388);
        feed2(11, 185, 388);
        CHECK(sense_pressure(11) == 64 || sense_pressure(11) == 63);   /* 32 below it, of 64 */
        /* released: two pulled-up samples above RELEASE (four in all: driven ones never release) */
        feed2(11, 1023, 1023);
        feed2(11, 1023, 1023);
        feed2(11, 1023, 1023);
        CHECK(!take(&e));
        feed2(11, 1023, 1023);
        CHECK(take(&e) && e.type == EV_OFF);
        CHECK(sn_key[11].dfloor == 420);           /* it landed lower than assumed: learned (the squeeze is not a landing) */
        CHECK(!sense_want_drive(11));
        settle(11, 10);
        drain();
        /* a soft press: the driven reading sits 300 above the floor (388) */
        feed2(11, 300, 999);
        feed2(11, 260, 690);
        feed2(11, 240, 690);
        CHECK(take(&e) && e.type == EV_ON);
        v_soft = e.vel;
        printf("  driven: hard %u  soft %u\n", v_hard, v_soft);
        CHECK(v_soft < 75 && v_soft > 45);
        feed2(11, 1023, 1023);
        feed2(11, 1023, 1023);
        feed2(11, 1023, 1023);
        feed2(11, 1023, 1023);
        CHECK(take(&e) && e.type == EV_OFF);
        /* WAIT 2: two driven samples; the velocity is the lowest (the first caught the fall: 587, measured) */
        sn_cfg.wait = 2;
        settle(11, 10);
        drain();
        feed2(11, 181, 999);                       /* touch */
        feed2(11, 168, 600);                       /* driven: still falling */
        feed2(11, 167, 999);                       /* pulled: seated, but WAIT 2 */
        CHECK(!take(&e));
        feed2(11, 167, 400);                       /* driven: landed, hard (the floor is 388) */
        feed2(11, 167, 999);
        CHECK(take(&e) && e.type == EV_ON && e.vel > 110);
        CHECK((sn_cap_w.vdec & 0x3FFu) == 400);
        feed2(11, 1023, 1023);
        feed2(11, 1023, 1023);
        feed2(11, 1023, 1023);
        feed2(11, 1023, 1023);
        CHECK(take(&e) && e.type == EV_OFF);
        sn_cfg.wait = 1;
        /* a graze with drive on: the driven sample reads open too; still a graze, no note */
        settle(11, 10);
        feed2(11, 850, 1023);
        feed2(11, 850, 1023);
        feed2(11, 1023, 1023);
        CHECK(take(&e) && e.type == EV_GRAZE);
        CHECK(!take(&e));
        /* another strength: the driven floors start over */
        sense_forget_dfloor();
        CHECK(sn_key[11].dfloor == SN_DFLOOR_DEF);
        sn_cfg.drive = 2;                          /* each level starts from its own default floor */
        sense_forget_dfloor();
        CHECK(sn_key[11].dfloor == 600);
        sn_cfg.drive = 1;
        sn_cfg.drive = 0;
        settle(11, 4);
        CHECK(!sense_want_drive(11));
        drain();
    }

    /* the driven log scale (floor 400, LIGHTEST 1000, curve 0.7) on the measured readings: a hard hit
     * (~410) near 127, a regular press (~550) mid, a light one (~800) low but audible; 127 at and
     * below the floor, 1 at LIGHTEST; monotonic; a steeper curve is softer */
    {
        uint32_t v, last = 128;
        sn_cfg.d_curve = 70;
        sn_cfg.d_ceil = 1000;
        printf("  log scale: 410 -> %u, 550 -> %u, 800 -> %u, 950 -> %u\n", sense_velocity_d(410, 400, 1000),
               sense_velocity_d(550, 400, 1000), sense_velocity_d(800, 400, 1000), sense_velocity_d(950, 400, 1000));
        CHECK(sense_velocity_d(380, 400, 1000) == 127 && sense_velocity_d(400, 400, 1000) == 127);
        CHECK(sense_velocity_d(410, 400, 1000) > 110);
        CHECK(sense_velocity_d(550, 400, 1000) > 75 && sense_velocity_d(550, 400, 1000) < 100);
        CHECK(sense_velocity_d(800, 400, 1000) > 30 && sense_velocity_d(800, 400, 1000) < 55);
        CHECK(sense_velocity_d(1000, 400, 1000) == 1 && sense_velocity_d(1023, 400, 1000) == 1);
        for (v = 380; v <= 1023; v += 3) {
            uint8_t x = sense_velocity_d(v, 400, 1000);
            CHECK(x <= last);
            last = x;
        }
        sn_cfg.d_curve = 130;
        CHECK(sense_velocity_d(550, 400, 1000) < 75);
        sn_cfg.d_curve = 70;
    }

    /* calibration: a key's floor and ceiling settle on its own firm and light landings (DRIVE 2's,
     * measured: firm 547 +- 5, light ~780 +- 40), from the level's defaults; a freak press moves an
     * end by at most a step; the ends stay apart; reset goes back to the defaults */
    {
        int i;
        sense_key_t *k = &sn_key[9];
        sn_cfg.drive = 2;
        sn_cfg.d_ceil = 1000;
        sense_cal_reset();
        CHECK(k->dfloor == 600 && k->dceil == 1000 && k->nland == 0);
        srand(7);
        for (i = 0; i < 60; i++)
            sense_learn(k, i % 2 ? 740u + (uint32_t)(rand() % 80) : 542u + (uint32_t)(rand() % 11));
        printf("  calibrated: floor %u ceiling %u (from 600 / 1000)\n", k->dfloor, k->dceil);
        CHECK(k->dfloor >= 535 && k->dfloor <= 560);
        CHECK(k->dceil >= 790 && k->dceil <= 860);
        CHECK(sense_velocity_d(545, k->dfloor, k->dceil) > 115);    /* a firm press near 127 */
        CHECK(sense_velocity_d(810, k->dfloor, k->dceil) < 25);     /* a light one near the bottom */
        {
            uint16_t f0 = k->dfloor;
            sense_learn(k, 300);                   /* a freak: one step at most */
            CHECK(f0 - k->dfloor <= 24);
        }
        sn_cal_put(k, 700u << 4, 720u << 4);       /* too close: parted */
        CHECK(k->dceil - k->dfloor >= SN_SPAN);
        sense_cal_reset();
        CHECK(k->dfloor == 600 && k->dceil == 1000);
        sn_cfg.drive = 1;
        sense_cal_reset();
    }

    /* the top-row probe: a baseline while open, the peak above it while pressed; t_lo = 1, t_hi = 127
     * (measured: contact ~20, a light hold 25..34, a squeeze 40..46) */
    {
        int i;
        sn_cfg.t_lo = 20;
        sn_cfg.t_hi = 48;
        for (i = 0; i < 40; i++)
            sense_probe(22, 600, 0);
        CHECK(sn_probe[22].base == 600);
        CHECK(sense_probe_velocity(22) == 0);         /* not pressed: no reading */
        sense_probe(22, 620, 1);
        sense_probe(22, 634, 1);
        sense_probe(22, 628, 1);
        CHECK(sn_probe[22].peak == 34);
        CHECK(sense_probe_velocity(22) == 64);        /* halfway from 20 to 48 */
        sense_probe(22, 660, 1);
        CHECK(sense_probe_velocity(22) == 127);
        sense_probe(22, 601, 0);
        CHECK(sense_probe_velocity(22) == 0 && sn_probe[22].peak == 0);
        sense_probe(22, 615, 1);                    /* a light touch, below TOP LOW: the softest */
        CHECK(sense_probe_velocity(22) == 1);
    }

    /* keys off the analog rows are not analog */
    CHECK(sense_is_analog(0) && !sense_is_analog(20));

    printf(fails ? "sense: %d FAILED\n" : "sense: ok\n", fails);
    return fails ? 1 : 0;
}
