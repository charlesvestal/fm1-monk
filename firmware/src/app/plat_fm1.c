/* SPDX-License-Identifier: GPL-3.0-only */
/* MONK-1 (from Velo) platform on the FM-1: plat.h over Felucca's HAL, panel map, USB rings and storage.
 * Part of the unity build (main.c), after the HAL, usb.c, storage.c and panel.c. */

uint32_t plat_ms(void) { return fm1_ms; }

uint32_t plat_buttons(void)
{
    uint32_t raw = fm1_in.buttons, out = 0, i;
    for (i = 0; i < NB; i++)
        if (raw & (1u << panel.btn[i]))
            out |= 1u << i;
    return out;
}

uint32_t plat_keys(void) { return fm1_in.notes & ((1u << NKEYS) - 1u); }

int32_t plat_enc(int role)
{
    if (role < 0 || role >= NE)
        return 0;
    return fm1_enc_take(panel.enc[role]) * panel.dir[role];
}

static volatile uint32_t master_q12 = 2048;
uint32_t plat_master(void) { return master_q12; }

/* LEDs: the picture is built off-line and copied one byte per column (Felucca: clearing
 * and relighting would let the 10 kHz scan catch the dark gap and flicker) */
static uint8_t led_pos[FM1_NKEY];
static void led_pos_init(void)
{
    uint32_t id, p, r;
    for (id = 0; id < FM1_NKEY; id++) {
        led_pos[id] = 0xFF;
        for (p = 0; p < FM1_NCOL; p++)
            for (r = 1; r < 5u; r++)
                if (FM1_KEYMAP[r][p] == (int8_t)id)
                    led_pos[id] = (uint8_t)((p << 3) | r);
    }
}
/* Glow (FoMni's): every button not lit is lit dim, on one scan frame in GLOW_DIV. PLAY has two LEDs: its
 * red at the button's own position, a green at the keyless matrix position column 8, row PA5: lit, PLAY
 * shows green */
#define GLOW_DIV 4u
void plat_leds(uint32_t buttons, uint32_t keys)
{
    uint8_t nl[FM1_NCOL] = {0}, dl[FM1_NCOL] = {0};
    uint32_t i, c;
    for (i = 0; i < NB + NKEYS; i++) {
        uint32_t id = i < NB ? panel.btn[i] : 14u + (i - NB);
        int on = i < NB ? (buttons >> i) & 1u : (keys >> (i - NB)) & 1u;
        uint8_t q = led_pos[id];
        if (i == B_PLAY)
            q = (uint8_t)((8u << 3) | 1u);
        if (q == 0xFF)
            continue;
        if (on)
            nl[q >> 3] |= (uint8_t)(1u << (q & 7u));
        else if (i < NB)
            dl[q >> 3] |= (uint8_t)(1u << (q & 7u));
    }
    for (c = 0; c < FM1_NCOL; c++) {
        fm1_led[c] = nl[c];
        fm1_led_dim[c] = dl[c];
    }
    fm1_led_dim_div = GLOW_DIV;
}

/* MIDI: Felucca's rings carry 4-byte USB-MIDI event packets, byte 0 = cable / CIN */
int plat_midi_in(uint32_t *pkt)
{
    if (mi_r == mi_w)
        return 0;
    *pkt = midi_in_q[mi_r % MQ];
    mi_r++;
    return 1;
}
void plat_midi_out(uint32_t pkt) { midi_out_event(pkt); }
/* a channel message: a USB-MIDI packet, cable 0, CIN = the status's high nibble */
void plat_midi3(uint32_t st, uint32_t d1, uint32_t d2) { plat_midi_out(st >> 4 | st << 8 | d1 << 16 | d2 << 24); }
void plat_sysex(const uint8_t *d, uint32_t n)          /* d[0] = F0 .. d[n-1] = F7 */
{
    while (n > 3u) {
        plat_midi_out(0x04u | (uint32_t)d[0] << 8 | (uint32_t)d[1] << 16 | (uint32_t)d[2] << 24);
        d += 3;
        n -= 3u;
    }
    if (n == 3u)
        plat_midi_out(0x07u | (uint32_t)d[0] << 8 | (uint32_t)d[1] << 16 | (uint32_t)d[2] << 24);
    else if (n == 2u)
        plat_midi_out(0x06u | (uint32_t)d[0] << 8 | (uint32_t)d[1] << 16);
    else if (n == 1u)
        plat_midi_out(0x05u | (uint32_t)d[0] << 8);
}

uint32_t plat_midi_dropped(void) { return mo_dropped; }

/* the analog rows: the scan tick calls this with each sample (main.c's FM1_INPUT_ANALOG) */
/* the key on this row of this column is open: its column's LEDs may be lit while it is read (fm1_input.h) */
static int plat_analog_led_ok(uint32_t col, uint32_t rowbit)
{
    int id = FM1_KEYMAP[rowbit][col];
    uint32_t k = id >= 14 ? (uint32_t)id - 14u : 0xFFu;
    return k >= NKEYS || sn_key[k].st == SN_OPEN;
}
static void plat_analog_sample(uint32_t col, uint32_t rowbit, uint32_t v, uint32_t driven)
{
    int id = FM1_KEYMAP[rowbit][col];
    if (id >= 14)
        sense_sample((uint32_t)id - 14u, v, fm1_ticks(), (int)driven);
}
/* the scan asks before each row sample whether to take it driven (sense.c decides per key) */
static int plat_analog_drive(uint32_t col, uint32_t rowbit)
{
    int id = FM1_KEYMAP[rowbit][col];
    return id >= 14 && sense_want_drive((uint32_t)id - 14u);
}
/* a probe reading of the key at (col, rowbit) */
static void plat_probe_sample(uint32_t col, uint32_t rowbit, uint32_t v, uint32_t pressed)
{
    int id = FM1_KEYMAP[rowbit][col];
    if (id >= 14)
        sense_probe((uint32_t)id - 14u, v, pressed);
}
uint32_t plat_analog_mask(void)
{
    uint32_t m = 0, c, r;
    for (c = 0; c < FM1_NCOL; c++)
        for (r = 3; r <= 4u; r++)
            if (FM1_KEYMAP[r][c] >= 14)
                m |= 1u << (FM1_KEYMAP[r][c] - 14);
    return m;
}
void plat_analog_config(uint32_t div, uint32_t delay, uint32_t immediate, uint32_t drive, uint32_t probe)
{
    fm1_an.immediate = (uint8_t)immediate;
    fm1_an.drive = (uint8_t)(drive > 4u ? 4u : drive);
    fm1_an.probe = (uint8_t)(probe & 7u);
    fm1_an.con = (uint16_t)FM1_ADC_CON(div, delay);
}
void plat_analog_selftest(uint32_t on) { fm1_an.selftest = (uint8_t)on; }
uint32_t plat_analog_us(void)
{
    static uint32_t last;
    if (fm1_ms - last > 1000u) {
        last = fm1_ms;
        fm1_an.time_req = 1;
    }
    return fm1_an.us;
}
uint32_t plat_analog_timeouts(void) { return fm1_an.timeouts; }
uint32_t plat_cap_probe(uint32_t row, uint32_t n)
{
    uint32_t i, sum = 0, f = irq_save();
    for (i = 0; i < n; i++)
        sum += fm1_cap_probe(row ? 4u : 3u);
    irq_restore(f);
    return n ? sum / n : 0;
}

int plat_store_load(uint32_t obj, void *dst, uint32_t max) { return flash_ok ? st_load(obj, dst, max) : -1; }
int plat_store_save(uint32_t obj, const void *src, uint32_t len) { return flash_ok ? st_save(obj, src, len) : -9; }

static volatile uint32_t audio_cpu_pct, audio_xruns;
uint32_t plat_cpu_pct(void) { return audio_cpu_pct; }
uint32_t plat_xruns(void) { return audio_xruns; }
