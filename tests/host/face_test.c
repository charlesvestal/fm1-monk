/* SPDX-License-Identifier: GPL-3.0-only */
/* The characters as the firmware draws them (app/face.c): the scene and the face unpacked, the mouth
 * rasterized, for the poses of the iOS reference frames (assets/characters/ref). Writes each frame as
 * raw RGB565 (big-endian, as the panel takes it) for tests/face_compare.py, which compares them
 * with the iOS port's own renders. Also times a frame. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#define OM_POOL
#include "art.h"
#include "../../firmware/src/app/inflate.c"
#include "../../firmware/src/app/face.c"

static uint16_t full[240 * 240], reg[16384];
static const int REFS[][2] = {{0, 0}, {0, 4}, {12, 0}, {12, 4}, {17, 2}, {23, 4}};

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    int c, r, y;
    clock_t t0;
    for (c = 0; c < ART_N; c++) {
        const art_char_t *a = &ART[c];
        for (r = 0; r < 6; r++) {
            char path[256];
            FILE *f;
            face_pose_t p;
            p.vowel = (uint8_t)REFS[r][0];
            p.loud = (uint8_t)REFS[r][1];
            p.blink = 0;
            face_scene(a, full, 0, 0, 240, 240);
            face_draw(a, p, reg);
            for (y = 0; y < a->rh; y++)
                memcpy(&full[(a->ry + y) * 240 + a->rx], &reg[y * a->rw], (size_t)a->rw * 2u);
            snprintf(path, sizeof path, "%s/%s_v%d_l%d.565", dir, a->id, REFS[r][0], REFS[r][1]);
            f = fopen(path, "wb");
            if (!f)
                return 1;
            fwrite(full, 2, 240 * 240, f);
            fclose(f);
        }
    }
    t0 = clock();
    for (r = 0; r < 200; r++) {
        face_pose_t p = {(uint8_t)(r % 24), (uint8_t)(r % 5), 0};
        face_draw(&ART[r % ART_N], p, reg);
    }
    printf("face: %.0f us a frame on this host\n", (double)(clock() - t0) * 1e6 / CLOCKS_PER_SEC / 200.0);
    return 0;
}
