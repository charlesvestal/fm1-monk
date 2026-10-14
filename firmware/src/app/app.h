/* SPDX-License-Identifier: GPL-3.0-only */
/* MONK-1 app: the settings (everything that is saved) and the app's entry points. Included by the
 * unity build after gfx.c (drawing helpers in scope). */
#pragma once
#include <stdint.h>
#include "plat.h"
#include "../dsp/monk.h"
#include "art.h"

#ifndef OM_VERSION                       /* a release build passes its own (tools/build.py --release) */
#define OM_VERSION "DEV"
#endif
#define PROJ_MAGIC 0x4B4E4F4Du           /* "MONK" */
#define PROJ_FORMAT 3u                   /* 2: SENSITIVE in VELOCITY's place; 3: VELOCITY back (2 loads, at 50) */

typedef struct {
    uint32_t magic, format;
    uint8_t character;                   /* 0 .. ART_N - 1 */
    uint8_t octave;                      /* 0 .. 4: F3 is MIDI 53 + 12 * (octave - 2) */
    uint8_t page;
    uint8_t at_route;                    /* MK_AT_*: where pressure goes */
    uint8_t at_depth;                    /* 0..100: how far full pressure moves the vowel */
    uint8_t p_sens;                      /* 0..100: pressure's sensitivity (how far a lean goes for 127) */
    uint8_t vel_sens;                    /* 0..100: how much velocity changes the level */
    uint8_t rsv[9];
    float p[ART_N][MK_NPARAM];           /* every character's voice as last left (0..1) */
} project_t;

extern project_t proj;

void project_defaults(void);
void project_reset_character(uint32_t c);   /* the character's voice as the iOS port has it */
int project_load(void);                  /* 0 = loaded; else defaults are in place */
int project_save(void);                  /* 0 = saved and verified */
void project_apply(void);                /* the current character's voice and the touch to the engine */
void sense_setup(void);                  /* Velo's sensing, as it was measured best (not saved) */

void ui_init(void);
void ui_frame(void);
void ui_input_only(void);
void ui_say(const char *a, const char *b);
