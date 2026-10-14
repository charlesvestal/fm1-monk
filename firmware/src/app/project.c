/* SPDX-License-Identifier: GPL-3.0-only */
/* MONK-1 settings: one flash object (OBJ_PROJ): the character, the octave, the touch, and every
 * character's voice as it was last left. Another format, or none, gives the defaults: each
 * character's voice as the iOS port loads it (art.h, from its CharacterVoiceTable / factory presets). */
project_t proj;

void project_reset_character(uint32_t c)
{
    uint32_t k;
    if (c >= ART_N)
        return;
    for (k = 0; k < MK_NPARAM; k++)
        proj.p[c][k] = ART[c].p[k];
}

void project_defaults(void)
{
    uint32_t c;
    memset(&proj, 0, sizeof proj);
    proj.magic = PROJ_MAGIC;
    proj.format = PROJ_FORMAT;
    proj.character = 0;
    proj.octave = 1;                     /* F2 .. G4: where the voices sit (upstream's pad: C3 .. C4) */
    proj.page = 0;
    proj.at_route = MK_AT_VOWEL;
    proj.at_depth = 50;                  /* the Schwung port's default */
    proj.p_sens = 50;
    proj.vel_sens = 50;
    for (c = 0; c < ART_N; c++)
        project_reset_character(c);
}

static int project_valid(void)
{
    uint32_t c, k;
    if (proj.magic != PROJ_MAGIC || (proj.format != PROJ_FORMAT && proj.format != 2u))
        return 0;
    if (proj.character >= ART_N || proj.octave > 4u || proj.at_route >= MK_NAT || proj.at_depth > 100u ||
        proj.p_sens > 100u || proj.vel_sens > 100u)
        return 0;
    for (c = 0; c < ART_N; c++)
        for (k = 0; k < MK_NPARAM; k++)
            if (!(proj.p[c][k] >= 0.0f && proj.p[c][k] <= 1.0f))   /* NaN fails too */
                return 0;
    return 1;
}

int project_load(void)
{
    int n;
    memset(&proj, 0, sizeof proj);
    n = plat_store_load(OBJ_PROJ, &proj, sizeof proj);
    if (n >= 8 && project_valid()) {
        if (proj.format == 2u) {         /* before VELOCITY came back */
            proj.format = PROJ_FORMAT;
            proj.vel_sens = 50;
        }
        return 0;
    }
    project_defaults();
    return -1;
}

int project_save(void) { return plat_store_save(OBJ_PROJ, &proj, sizeof proj); }

void project_apply(void)
{
    uint32_t k;
    for (k = 0; k < MK_NPARAM; k++)
        monk_param((int)k, proj.p[proj.character][k]);
    monk_touch(proj.at_route, (float)proj.at_depth * 0.01f, (float)proj.vel_sens * 0.01f);
}

/* Velo's sensing as its notes leave it (notes/01-lab.md there): the driven reading at DRIVE 2, the
 * deeper of two samples (WAIT 2), the log scale, the top row through the PA10 probe; the
 * calibration off. (The pressure is ui.c's: from where the key rests.) */
void sense_setup(void)
{
    sn_cfg.touch = 900;
    sn_cfg.press = 400;
    sn_cfg.release = 600;
    sn_cfg.wait = 2;
    sn_cfg.v_lo = 0;
    sn_cfg.v_hi = 72;
    sn_cfg.vmax = 127;
    sn_cfg.at_range = 32;
    sn_cfg.drive = 2;
    sn_cfg.d_curve = 70;
    sn_cfg.d_ceil = 896;
    sn_cfg.d_at_range = 64;
    sn_cfg.t_lo = 20;
    sn_cfg.t_hi = 48;
    sn_cfg.learn = 0;
    sense_cal_reset();
    plat_analog_config(7, 2, 0, 2, 1);   /* divider 7, delay 2 (faster returns rails), DRIVE 2, the top-row probe */
}
