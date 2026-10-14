/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* FM-1 SARADC: PB6 = MASTER pot (ch4), PB1 = battery divider (ch3), and two of the key matrix
 * rows: PA7 (ch0) and PA8 (ch1). 10 bit.
 *
 *   fm1_adc_init()               the pot and battery pins analog inputs, no pulls
 *   fm1_adc_read(ch)             blocking, 0..1023, -1 on timeout (the SDK's clock settings)
 *   fm1_adc_start(ch, con)       start one conversion; con = FM1_ADC_CON(div, delay)
 *   fm1_adc_done()               the conversion finished
 *   fm1_adc_take()               the result, and the converter idle again
 *   fm1_adc_abort()              give up on a conversion
 *
 * One converter: fm1_input.h's tick owns it (it samples the rows between two ticks and the pot
 * once a frame), so the main loop must not call fm1_adc_read while the scan runs.
 *
 * CON: bits 0-2 the clock divider (the SDK uses 6: "div 96"), bit 3 set, bit 4 enable, bit 5
 * interrupt enable, bit 6 start / clear pending, bit 7 pending, bits 8-11 the channel, bits
 * 12-15 the start delay in units of 8 ADC clocks (the SDK uses 15). A smaller divider and
 * delay make a conversion short enough to fit between two scan ticks (100 us). */
#pragma once
#include <stdint.h>
#include "fm1_gpio.h"

#define FM1_ADC_CON_R (*(volatile uint32_t *)0x13100u)
#define FM1_ADC_RES (*(volatile uint32_t *)0x13104u)
#define FM1_WLA_CON0 (*(volatile uint32_t *)0x11900u)
enum { FM1_ADC_ROW_PA7 = 0, FM1_ADC_ROW_PA8 = 1, FM1_ADC_BATT = 3, FM1_ADC_MASTER = 4 };
#define FM1_ADC_CON(div, delay) ((uint32_t)((div) & 7u) | 8u | ((uint32_t)((delay) & 15u) << 12))
#define FM1_ADC_CON_SDK FM1_ADC_CON(6, 15)

/* Both are only ever analog inputs: PB1 held low for 8 s would reset the
 * chip (isd_config). */
static void fm1_adc_init(void)
{
    uint32_t m = (1u << 6) | (1u << 1);
    FM1_PR(FM1_PB, FM1_DIE) &= ~m;
    FM1_PR(FM1_PB, FM1_PU) &= ~m;
    FM1_PR(FM1_PB, FM1_PD) &= ~m;
    FM1_PR(FM1_PB, FM1_DIR) |= m;
}

/* the matrix rows PA7 / PA8: digital inputs with pull-ups for the scan; the converter samples
 * the pad as it is. This switches their digital input buffers off (the scan then no longer sees
 * those rows) for the experiment that asks whether the buffer disturbs the reading. */
static void fm1_adc_rows_die(int on)
{
    uint32_t m = (1u << 7) | (1u << 8);
    if (on)
        FM1_PR(FM1_PA, FM1_DIE) |= m;
    else
        FM1_PR(FM1_PA, FM1_DIE) &= ~m;
}

static void fm1_adc_start(uint32_t ch, uint32_t con)
{
    FM1_ADC_CON_R = 0;
    if (FM1_WLA_CON0 & (1u << 14))
        FM1_WLA_CON0 &= ~(1u << 14);
    FM1_ADC_CON_R = (con & 0xF00Fu) | 0x40u | ((ch & 0xFu) << 8);
    FM1_ADC_CON_R |= 0x10u;
    FM1_ADC_CON_R |= 0x40u;
}
static inline int fm1_adc_done(void) { return (FM1_ADC_CON_R & 0x80u) != 0; }
static inline uint32_t fm1_adc_take(void)
{
    uint32_t v = FM1_ADC_RES & 0x3FFu;
    FM1_ADC_CON_R = 0x40u;
    FM1_ADC_CON_R = 0;
    return v;
}
static inline void fm1_adc_abort(void)
{
    FM1_ADC_CON_R = 0x40u;
    FM1_ADC_CON_R = 0;
}

static int32_t fm1_adc_read(uint32_t ch)
{
    uint32_t t;
    fm1_adc_start(ch, FM1_ADC_CON_SDK);
    for (t = 0; t < 20000u; t++)
        if (fm1_adc_done())
            break;
    if (t == 20000u) {
        fm1_adc_abort();
        return -1;
    }
    return (int32_t)fm1_adc_take();
}
