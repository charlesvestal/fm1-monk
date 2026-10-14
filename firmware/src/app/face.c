/* SPDX-License-Identifier: GPL-3.0-only */
/* The singing character (the iOS port's CharacterView, for a 240 x 240 screen with no framebuffer).
 *
 * The scene and the body come pre-rendered from the iOS drawing code (tools/gen_art.py: palette
 * images, deflated in bands), and so does the face in each of its states (eyes open or blinking, the
 * brows lifted by loudness in four steps). The mouth is drawn here, as the iOS ToonMouth draws it:
 * five anchors (OO, OH, AH, EH, EE) interpolated by the vowel, the aperture four cubics, lips, the
 * tongue and teeth clipped to it, an ink outline, the height swelling with loudness; and over it
 * what some characters wear there (a moustache, whiskers). The vowel moves in 24 frames and the
 * loudness in 4 steps, as on iOS: the stepped, puppet-like motion of the original's sprite sheet.
 *
 * Drawing goes into a caller's RGB565 buffer (byte-swapped, as the panel takes it): a rectangle of
 * the 240 x 240 scene. raster: a small anti-aliased path filler (4 sub-scanlines, exact coverage
 * across), nonzero winding; strokes are round-capped and -joined (a quad per segment, discs at the
 * ends and the corners); fills can be clipped to another path. No HAL here: the host tests run it. */
#include <stdint.h>
#include "../dsp/fastmath.h"

/* --------------------------------------------------------------------------- decoding */
typedef struct {
    uint16_t *buf;                     /* the target: rows y0 .. y0 + h - 1, columns x0 .. x0 + w - 1 */
    int32_t x0, y0, w, h;
    const uint16_t *pal;
    int32_t sx0, sy0, sw;              /* the source rectangle's origin and width (row-major bytes) */
    uint32_t pos, end;                 /* bytes emitted; stop after this many */
} fc_dec_t;

static int fc_put(void *ctx, uint8_t c)
{
    fc_dec_t *d = (fc_dec_t *)ctx;
    uint32_t p = d->pos++;
    int32_t x = d->sx0 + (int32_t)(p % (uint32_t)d->sw) - d->x0;
    int32_t y = d->sy0 + (int32_t)(p / (uint32_t)d->sw) - d->y0;
    if ((uint32_t)x < (uint32_t)d->w && (uint32_t)y < (uint32_t)d->h)
        d->buf[y * d->w + x] = d->pal[c];
    return d->pos >= d->end;
}

/* the scene (no face), rows [y0, y0 + h) and columns [x0, x0 + w), into buf */
static void face_scene(const art_char_t *a, uint16_t *buf, int32_t x0, int32_t y0, int32_t w, int32_t h)
{
    int32_t b;
    fc_dec_t d;
    d.buf = buf;
    d.x0 = x0;
    d.y0 = y0;
    d.w = w;
    d.h = h;
    d.pal = a->pal;
    d.sx0 = 0;
    d.sw = 240;
    for (b = y0 / ART_BAND; b < ART_NBANDS && b * ART_BAND < y0 + h; b++) {
        int32_t last = y0 + h - b * ART_BAND;             /* rows of this band needed */
        if (last > ART_BAND)
            last = ART_BAND;
        d.sy0 = b * ART_BAND;
        d.pos = 0;
        d.end = (uint32_t)(last * 240);
        inflate_mem(a->base + a->boff[b], a->boff[b + 1] - a->boff[b], fc_put, &d);
    }
}

/* the face in state s (blink * 5 + loudness step), over what the buffer holds */
static void face_patch(const art_char_t *a, uint32_t s, uint16_t *buf, int32_t x0, int32_t y0, int32_t w, int32_t h)
{
    uint32_t k = a->face_of[s % 10u];
    fc_dec_t d;
    d.buf = buf;
    d.x0 = x0;
    d.y0 = y0;
    d.w = w;
    d.h = h;
    d.pal = a->pal;
    d.sx0 = a->fx;
    d.sy0 = a->fy;
    d.sw = a->fw;
    d.pos = 0;
    d.end = (uint32_t)a->fw * a->fh;
    inflate_mem(a->face + a->foff[k], a->foff[k + 1] - a->foff[k], fc_put, &d);
}

/* --------------------------------------------------------------------------- raster */
#define RS_PT 1024                     /* source path points */
#define RS_SUB 32                      /* source subpaths */
#define RS_EDGE 1536                   /* polygon edges for one fill */
#define RS_COV 16384                   /* coverage / clip buffers: the largest region, gen_art checks */
#define RS_X 160                       /* crossings a sub-scanline (sorted): a stroke is many overlapping pieces */

static struct {
    float x[RS_PT], y[RS_PT];          /* the source path, stage units */
    uint16_t start[RS_SUB + 1];
    uint8_t closed[RS_SUB];
    int32_t n, nsub;
    float cx, cy, sx, sy;              /* pen, subpath start */
    /* the target */
    uint16_t *buf;
    int32_t x0, y0, w, h;
    float k;                           /* px per stage unit */
    float ox, oy;                      /* a translation for the next conversion (stage units) */
} R;

typedef struct { float x0, y0, x1, y1; } rs_edge_t;
static rs_edge_t rs_e[RS_EDGE];
static uint16_t rs_order[RS_EDGE];
static int32_t rs_ne;
static float rs_bx0, rs_by0, rs_bx1, rs_by1;   /* the edges' bounds, px */
static uint8_t rs_cov[RS_COV] OM_POOL, rs_clip[RS_COV] OM_POOL;
static int32_t rs_cx0, rs_cy0, rs_cx1, rs_cy1;   /* the coverage bounds written (cov) */
static int32_t rs_kx0, rs_ky0, rs_kx1, rs_ky1;   /* the clip's bounds */
static int16_t rs_acc[256];

static void rs_target(uint16_t *buf, int32_t x0, int32_t y0, int32_t w, int32_t h)
{
    R.buf = buf;
    R.x0 = x0;
    R.y0 = y0;
    R.w = w;
    R.h = h;
    R.k = 240.0f / 300.0f;
}

static void rs_begin(void)
{
    R.n = 0;
    R.nsub = 0;
    R.start[0] = 0;
    R.ox = R.oy = 0.0f;
}

static void rs_pt(float x, float y)
{
    if (R.n < RS_PT) {
        R.x[R.n] = x;
        R.y[R.n] = y;
        R.n++;
    }
    R.cx = x;
    R.cy = y;
}

static void rs_endsub(int closed)
{
    if (R.nsub < RS_SUB && R.n > (int32_t)R.start[R.nsub]) {
        R.closed[R.nsub] = (uint8_t)closed;
        R.nsub++;
        R.start[R.nsub] = (uint16_t)R.n;
    } else {
        R.n = R.start[R.nsub];
    }
}

static void rs_move(float x, float y)
{
    if (R.nsub < RS_SUB && R.n > (int32_t)R.start[R.nsub])
        rs_endsub(0);
    R.sx = x;
    R.sy = y;
    rs_pt(x, y);
}
static void rs_line(float x, float y) { rs_pt(x, y); }
static void rs_close(void)
{
    rs_endsub(1);
    R.cx = R.sx;
    R.cy = R.sy;
}

static int32_t rs_steps(float d)       /* segments for a curve about d stage units long */
{
    int32_t n = (int32_t)(d * R.k / 2.5f);
    return n < 4 ? 4 : n > 24 ? 24 : n;
}

static void rs_cubic(float x1, float y1, float x2, float y2, float x3, float y3)
{
    float x0 = R.cx, y0 = R.cy;
    int32_t i, n = rs_steps(fm_fabsf(x1 - x0) + fm_fabsf(y1 - y0) + fm_fabsf(x2 - x1) + fm_fabsf(y2 - y1) +
                            fm_fabsf(x3 - x2) + fm_fabsf(y3 - y2));
    for (i = 1; i <= n; i++) {
        float t = (float)i / (float)n, u = 1.0f - t;
        float a = u * u * u, b = 3.0f * u * u * t, c = 3.0f * u * t * t, d = t * t * t;
        rs_pt(a * x0 + b * x1 + c * x2 + d * x3, a * y0 + b * y1 + c * y2 + d * y3);
    }
}

static void rs_quad(float x1, float y1, float x2, float y2)
{
    float x0 = R.cx, y0 = R.cy;
    int32_t i, n = rs_steps(fm_fabsf(x1 - x0) + fm_fabsf(y1 - y0) + fm_fabsf(x2 - x1) + fm_fabsf(y2 - y1));
    for (i = 1; i <= n; i++) {
        float t = (float)i / (float)n, u = 1.0f - t;
        rs_pt(u * u * x0 + 2.0f * u * t * x1 + t * t * x2, u * u * y0 + 2.0f * u * t * y1 + t * t * y2);
    }
}

/* UIBezierPath(ovalIn:): four cubics, from the right, clockwise on screen */
static void rs_ellipse(float cx, float cy, float rx, float ry)
{
    float kx = rx * 0.5522848f, ky = ry * 0.5522848f;
    rs_move(cx + rx, cy);
    rs_cubic(cx + rx, cy + ky, cx + kx, cy + ry, cx, cy + ry);
    rs_cubic(cx - kx, cy + ry, cx - rx, cy + ky, cx - rx, cy);
    rs_cubic(cx - rx, cy - ky, cx - kx, cy - ry, cx, cy - ry);
    rs_cubic(cx + kx, cy - ry, cx + rx, cy - ky, cx + rx, cy);
    rs_close();
}

static void rs_rect(float x, float y, float w, float h)
{
    rs_move(x, y);
    rs_line(x + w, y);
    rs_line(x + w, y + h);
    rs_line(x, y + h);
    rs_close();
}

/* the absolute SVG subset the drawings use: M L C Q Z (Toon.path) */
static float rs_num(const char **s)
{
    const char *p = *s;
    float v = 0.0f, sc = 0.0f, sign = 1.0f;
    while (*p == ' ' || *p == ',')
        p++;
    if (*p == '-') {
        sign = -1.0f;
        p++;
    }
    for (; (*p >= '0' && *p <= '9') || *p == '.'; p++) {
        if (*p == '.')
            sc = 1.0f;
        else if (sc > 0.0f)
            v += (float)(*p - '0') * (sc *= 0.1f);
        else
            v = v * 10.0f + (float)(*p - '0');
    }
    *s = p;
    return v * sign;
}

static void rs_svg(const char *d)
{
    char cmd = 'M';
    for (;;) {
        float a, b, c, e, f, g;
        while (*d == ' ' || *d == ',')
            d++;
        if (!*d)
            break;
        if ((*d >= 'A' && *d <= 'Z'))
            cmd = *d++;
        switch (cmd) {
        case 'M': a = rs_num(&d); b = rs_num(&d); rs_move(a, b); cmd = 'L'; break;
        case 'L': a = rs_num(&d); b = rs_num(&d); rs_line(a, b); break;
        case 'Q': a = rs_num(&d); b = rs_num(&d); c = rs_num(&d); e = rs_num(&d); rs_quad(a, b, c, e); break;
        case 'C':
            a = rs_num(&d); b = rs_num(&d); c = rs_num(&d); e = rs_num(&d); f = rs_num(&d); g = rs_num(&d);
            rs_cubic(a, b, c, e, f, g);
            break;
        case 'Z': rs_close(); break;
        default: return;
        }
    }
    if (R.n > (int32_t)R.start[R.nsub])
        rs_endsub(0);
}

static void rs_finish(void)
{
    if (R.n > (int32_t)R.start[R.nsub])
        rs_endsub(0);
}

/* ---- edges (px, relative to the target) */
static void rs_edge(float x0, float y0, float x1, float y1)
{
    if (rs_ne >= RS_EDGE || y0 == y1)
        return;
    rs_e[rs_ne].x0 = x0;
    rs_e[rs_ne].y0 = y0;
    rs_e[rs_ne].x1 = x1;
    rs_e[rs_ne].y1 = y1;
    rs_ne++;
    rs_bx0 = fm_minf(rs_bx0, fm_minf(x0, x1));
    rs_bx1 = fm_maxf(rs_bx1, fm_maxf(x0, x1));
    rs_by0 = fm_minf(rs_by0, fm_minf(y0, y1));
    rs_by1 = fm_maxf(rs_by1, fm_maxf(y0, y1));
}

static void rs_edges_reset(void)
{
    rs_ne = 0;
    rs_bx0 = rs_by0 = 1e9f;
    rs_bx1 = rs_by1 = -1e9f;
}

static float rs_px(float x) { return (x + R.ox) * R.k - (float)R.x0; }
static float rs_py(float y) { return (y + R.oy) * R.k - (float)R.y0; }

/* a polygon of n points with a positive area (screen: clockwise), for strokes' unions */
static void rs_poly_pos(const float *x, const float *y, int32_t n)
{
    float a = 0.0f;
    int32_t i;
    for (i = 0; i < n; i++) {
        int32_t j = (i + 1) % n;
        a += x[i] * y[j] - x[j] * y[i];
    }
    for (i = 0; i < n; i++) {
        int32_t j = (i + 1) % n;
        if (a >= 0.0f)
            rs_edge(x[i], y[i], x[j], y[j]);
        else
            rs_edge(x[j], y[j], x[i], y[i]);
    }
}

static void rs_disc(float cx, float cy, float r)
{
    float x[16], y[16];
    int32_t i, n = r < 2.0f ? 8 : r < 5.0f ? 12 : 16;
    for (i = 0; i < n; i++) {
        float t = FM_TWO_PI * (float)i / (float)n;
        x[i] = cx + r * fm_cosf(t);
        y[i] = cy + r * fm_sinf(t);
    }
    rs_poly_pos(x, y, n);
}

/* the path as fill edges */
static void rs_fill_edges(void)
{
    int32_t s, i;
    rs_finish();
    rs_edges_reset();
    for (s = 0; s < R.nsub; s++) {
        int32_t a = R.start[s], b = R.start[s + 1];
        for (i = a; i < b; i++) {
            int32_t j = i + 1 < b ? i + 1 : a;
            rs_edge(rs_px(R.x[i]), rs_py(R.y[i]), rs_px(R.x[j]), rs_py(R.y[j]));
        }
    }
}

/* the path's outline, round caps and joins, `width` stage units wide, as edges */
static void rs_stroke_edges(float width)
{
    float hw = width * R.k * 0.5f;
    int32_t s, i;
    rs_finish();
    rs_edges_reset();
    for (s = 0; s < R.nsub; s++) {
        int32_t a = R.start[s], b = R.start[s + 1], closed = R.closed[s], m = b - a;
        int32_t segs = closed ? m : m - 1;
        for (i = 0; i < segs; i++) {
            int32_t p = a + i, q = a + (i + 1) % m;
            float x0 = rs_px(R.x[p]), y0 = rs_py(R.y[p]), x1 = rs_px(R.x[q]), y1 = rs_py(R.y[q]);
            float dx = x1 - x0, dy = y1 - y0, l = fm_sqrtf(dx * dx + dy * dy), nx, ny;
            float qx[4], qy[4];
            if (l < 1e-4f)
                continue;
            nx = -dy / l * hw;
            ny = dx / l * hw;
            qx[0] = x0 + nx; qy[0] = y0 + ny;
            qx[1] = x1 + nx; qy[1] = y1 + ny;
            qx[2] = x1 - nx; qy[2] = y1 - ny;
            qx[3] = x0 - nx; qy[3] = y0 - ny;
            rs_poly_pos(qx, qy, 4);
        }
        for (i = 0; i < m; i++) {           /* joins: discs at the ends and sharp turns, bevels elsewhere */
            int32_t p = a + i, pp = a + (i + m - 1) % m, pn = a + (i + 1) % m, inner = closed || (i > 0 && i < m - 1);
            float px = rs_px(R.x[p]), py = rs_py(R.y[p]);
            if (inner && m > 2) {
                float ax = R.x[p] - R.x[pp], ay = R.y[p] - R.y[pp], bx = R.x[pn] - R.x[p], by = R.y[pn] - R.y[p];
                float la = fm_sqrtf(ax * ax + ay * ay), lb = fm_sqrtf(bx * bx + by * by), dot = ax * bx + ay * by;
                if (la > 1e-4f && lb > 1e-4f && dot > 0.96f * la * lb) {   /* under ~16 degrees: a bevel */
                    float tx[3], ty[3], n1x = -ay / la * hw, n1y = ax / la * hw, n2x = -by / lb * hw, n2y = bx / lb * hw;
                    tx[0] = px; ty[0] = py;
                    tx[1] = px + n1x; ty[1] = py + n1y;
                    tx[2] = px + n2x; ty[2] = py + n2y;
                    rs_poly_pos(tx, ty, 3);
                    tx[1] = px - n1x; ty[1] = py - n1y;
                    tx[2] = px - n2x; ty[2] = py - n2y;
                    rs_poly_pos(tx, ty, 3);
                    continue;
                }
            }
            rs_disc(px, py, hw);
        }
    }
}

/* one sub-scanline's span [a, b) (px from the scan's left edge) into the row's coverage, 64 a pixel */
static void rs_span(float a, float b, int32_t w)
{
    int32_t pa, pb, p;
    if (a < 0.0f) a = 0.0f;
    if (b > (float)w) b = (float)w;
    if (a >= b)
        return;
    pa = (int32_t)a;
    pb = (int32_t)b;
    if (pa == pb) {
        rs_acc[pa] = (int16_t)(rs_acc[pa] + (int32_t)((b - a) * 64.0f + 0.5f));
        return;
    }
    rs_acc[pa] = (int16_t)(rs_acc[pa] + (int32_t)(((float)(pa + 1) - a) * 64.0f + 0.5f));
    for (p = pa + 1; p < pb; p++)
        rs_acc[p] = (int16_t)(rs_acc[p] + 64);
    if (pb < w)
        rs_acc[pb] = (int16_t)(rs_acc[pb] + (int32_t)((b - (float)pb) * 64.0f + 0.5f));
}

/* the edges' coverage into cov (0..255), over their bounds clipped to the target; returns 0 if empty */
static int rs_scan(uint8_t *cov, int32_t *ox0, int32_t *oy0, int32_t *ox1, int32_t *oy1)
{
    int32_t x0 = (int32_t)fm_floorf(rs_bx0), x1 = (int32_t)fm_floorf(rs_bx1) + 1;
    int32_t y0 = (int32_t)fm_floorf(rs_by0), y1 = (int32_t)fm_floorf(rs_by1) + 1;
    int32_t y, i, j, act = 0, w;
    if (rs_ne == 0)
        return 0;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > R.w) x1 = R.w;
    if (y1 > R.h) y1 = R.h;
    if (x0 >= x1 || y0 >= y1 || (x1 - x0) > 256 || R.w * R.h > RS_COV)
        return 0;
    w = x1 - x0;
    for (i = 0; i < rs_ne; i++)             /* by the top end (insertion sort: a few hundred edges) */
        rs_order[i] = (uint16_t)i;
    for (i = 1; i < rs_ne; i++) {
        uint16_t t = rs_order[i];
        float ty = fm_minf(rs_e[t].y0, rs_e[t].y1);
        for (j = i - 1; j >= 0 && fm_minf(rs_e[rs_order[j]].y0, rs_e[rs_order[j]].y1) > ty; j--)
            rs_order[j + 1] = rs_order[j];
        rs_order[j + 1] = t;
    }
    {
        static uint16_t active[RS_EDGE];
        int32_t next = 0;
        for (y = y0; y < y1; y++) {
            int32_t sub;
            for (i = 0; i < w; i++)
                rs_acc[i] = 0;
            for (sub = 0; sub < 4; sub++) {
                float yy = (float)y + 0.125f + 0.25f * (float)sub, xs[RS_X];
                int8_t ds[RS_X];
                int32_t nx = 0, k, wind;
                float xa = 0.0f, xb;
                while (next < rs_ne && fm_minf(rs_e[rs_order[next]].y0, rs_e[rs_order[next]].y1) <= yy)
                    active[act++] = rs_order[next++];
                for (k = 0; k < act;) {          /* drop the finished, cross the rest */
                    const rs_edge_t *e = &rs_e[active[k]];
                    float lo = fm_minf(e->y0, e->y1), hi = fm_maxf(e->y0, e->y1);
                    if (hi <= yy) {
                        active[k] = active[--act];
                        continue;
                    }
                    if (lo <= yy && nx < RS_X) {
                        float x = e->x0 + (yy - e->y0) * (e->x1 - e->x0) / (e->y1 - e->y0);
                        int8_t d = e->y1 > e->y0 ? 1 : -1;
                        int32_t m = nx++;
                        while (m > 0 && xs[m - 1] > x) {
                            xs[m] = xs[m - 1];
                            ds[m] = ds[m - 1];
                            m--;
                        }
                        xs[m] = x;
                        ds[m] = d;
                    }
                    k++;
                }
                wind = 0;
                xb = -1e9f;
                for (k = 0; k < nx; k++) {
                    int32_t was = wind;
                    wind += ds[k];
                    if (was == 0 && wind != 0) {
                        if (xs[k] - xb > 1e-3f) {      /* a new span (else: the last one goes on) */
                            if (xb > -1e8f)
                                rs_span(xa - (float)x0, xb - (float)x0, w);
                            xa = xs[k];
                        }
                    } else if (was != 0 && wind == 0) {
                        xb = xs[k];
                    }
                }
                if (xb > -1e8f)
                    rs_span(xa - (float)x0, xb - (float)x0, w);
            }
            for (i = 0; i < w; i++) {
                int32_t c = rs_acc[i] >= 256 ? 255 : rs_acc[i];
                cov[(y * R.w) + x0 + i] = (uint8_t)c;
            }
        }
    }
    *ox0 = x0;
    *oy0 = y0;
    *ox1 = x1;
    *oy1 = y1;
    return 1;
}

static inline uint16_t rs_sw(uint32_t c) { return (uint16_t)(((c >> 8) & 0xFFu) | ((c & 0xFFu) << 8)); }

/* paint colour c (RGB565) with alpha (0..255) through the coverage, optionally through the clip */
static void rs_paint(uint32_t c, uint32_t alpha, int clipped)
{
    int32_t x, y, r = (int32_t)(c >> 11), g = (int32_t)((c >> 5) & 63u), b = (int32_t)(c & 31u);
    for (y = rs_cy0; y < rs_cy1; y++)
        for (x = rs_cx0; x < rs_cx1; x++) {
            uint32_t i = (uint32_t)(y * R.w + x), a = rs_cov[i];
            if (clipped)
                a = (x >= rs_kx0 && x < rs_kx1 && y >= rs_ky0 && y < rs_ky1) ? a * rs_clip[i] / 255u : 0u;
            a = a * alpha / 255u;
            if (!a)
                continue;
            if (a >= 255u) {
                R.buf[i] = rs_sw(c);
            } else {
                uint32_t d = rs_sw(R.buf[i]);
                int32_t dr = (int32_t)(d >> 11), dg = (int32_t)((d >> 5) & 63u), db = (int32_t)(d & 31u), ai = (int32_t)a;
                dr += ((r - dr) * ai + 128) / 256;
                dg += ((g - dg) * ai + 128) / 256;
                db += ((b - db) * ai + 128) / 256;
                R.buf[i] = rs_sw(((uint32_t)dr << 11) | ((uint32_t)dg << 5) | (uint32_t)db);
            }
        }
}

static void rs_fill(uint32_t c, uint32_t alpha, int clipped)
{
    rs_fill_edges();
    if (rs_scan(rs_cov, &rs_cx0, &rs_cy0, &rs_cx1, &rs_cy1))
        rs_paint(c, alpha, clipped);
}

static void rs_stroke(float width, uint32_t c)
{
    rs_stroke_edges(width);
    if (rs_scan(rs_cov, &rs_cx0, &rs_cy0, &rs_cx1, &rs_cy1))
        rs_paint(c, 255u, 0);
}

/* the current path becomes the clip (Toon.clipped) */
static void rs_set_clip(void)
{
    int32_t i;
    for (i = 0; i < R.w * R.h && i < RS_COV; i++)
        rs_clip[i] = 0;
    rs_fill_edges();
    if (!rs_scan(rs_clip, &rs_kx0, &rs_ky0, &rs_kx1, &rs_ky1))
        rs_kx0 = rs_kx1 = rs_ky0 = rs_ky1 = 0;
}

/* --------------------------------------------------------------------------- the Toon kit */
#define TOON_INK 0x28E3u               /* 2B1D1A */
#define TOON_BOLD 7.0f
#define TOON_MEDIUM 5.0f
#define TOON_FINE 3.5f
#define C565(h) ((uint32_t)((((h) >> 19) & 31u) << 11 | (((h) >> 10) & 63u) << 5 | (((h) >> 3) & 31u)))

/* Toon.shape: the fill's shadow tone, the fill moved up-left (what it uncovers is the shadow), ink */
static void toon_shape(const char *d, uint32_t fill, uint32_t shadow, float lw)
{
    rs_begin();
    rs_svg(d);
    rs_fill(shadow, 255u, 0);
    rs_set_clip();
    R.ox = -9.0f;
    R.oy = -8.0f;
    rs_fill(fill, 255u, 1);
    R.ox = R.oy = 0.0f;
    rs_stroke(lw, TOON_INK);
}

static void toon_line(const char *d, float w)
{
    rs_begin();
    rs_svg(d);
    rs_stroke(w, TOON_INK);
}

/* ToonMouth: OO, OH, AH, EH, EE */
static const float MOUTH_ANCHORS[5][6] = {   /* w, h, round, tongue, top, bottom */
    {24, 26, 1.0f, 0.0f, 0, 0},
    {34, 40, 1.0f, 0.3f, 0, 0},
    {50, 56, 0.9f, 1.0f, 0, 0},
    {64, 34, 0.45f, 0.8f, 1, 0},
    {74, 18, 0.12f, 0.0f, 1, 1},
};

static void mouth_path(float cx, float cy, float w, float h, float round)
{
    float l = cx - w / 2.0f, r = cx + w / 2.0f, t = cy - h * 0.42f, b = cy + h * 0.58f;
    float kx = (w / 2.0f) * 0.552f, kyt = h * 0.42f * 0.552f * round, kyb = h * 0.58f * 0.552f * round;
    rs_begin();
    rs_move(l, cy);
    rs_cubic(l, cy - kyt, cx - kx, t, cx, t);
    rs_cubic(cx + kx, t, r, cy - kyt, r, cy);
    rs_cubic(r, cy + kyb, cx + kx, b, cx, b);
    rs_cubic(cx - kx, b, l, cy + kyb, l, cy);
    rs_close();
}

static void toon_mouth(const art_char_t *a, float vowel, float boost)
{
    float t = fm_clampf(vowel, 0.0f, 1.0f) * 4.0f, f, p[6];
    int32_t i = (int32_t)t, k;
    float s = a->ms, w, h, cx = a->mx, cy = a->my;
    if (i > 3)
        i = 3;
    f = t - (float)i;
    for (k = 0; k < 6; k++)
        p[k] = MOUTH_ANCHORS[i][k] + (MOUTH_ANCHORS[i + 1][k] - MOUTH_ANCHORS[i][k]) * f;
    w = p[0] * s;
    h = p[1] * s * boost;
    if (a->variant == ART_LIPS) {
        mouth_path(cx, cy, w, h, p[2]);
        rs_stroke(17.0f * s, TOON_INK);
        mouth_path(cx, cy, w, h, p[2]);
        rs_stroke(10.0f * s, a->lip);
    }
    mouth_path(cx, cy, w, h, p[2]);
    rs_fill(C565(0x4A1622u), 255u, 0);
    rs_set_clip();
    if (p[3] > 0.02f) {
        rs_begin();
        rs_ellipse(cx + w * 0.08f, cy + h * 0.58f, w * 0.38f, h * 0.42f * p[3]);
        rs_fill(C565(0xE8607Au), 255u, 1);
    }
    if (p[4] > 0.02f) {
        rs_begin();
        rs_rect(cx - w, cy - h, w * 2.0f, h * 0.42f + h * 0.3f * p[4] - h * 0.12f);
        rs_fill(C565(0xFFFAF0u), 255u, 1);
    }
    if (p[5] > 0.02f) {
        rs_begin();
        rs_rect(cx - w, cy + h * 0.58f - h * 0.28f * p[5], w * 2.0f, h);
        rs_fill(C565(0xFFFAF0u), 255u, 1);
    }
    mouth_path(cx, cy, w, h, p[2]);
    rs_stroke((a->variant == ART_LIPS ? TOON_FINE : TOON_MEDIUM) * fm_maxf(0.8f, s), TOON_INK);
}

/* drawOverMouth: the old man's and the fire fighter's moustaches, the cat's whiskers */
static void toon_over(const art_char_t *a)
{
    if (a->id[0] == 'o') {             /* oldman */
        toon_shape("M108 162 C120 148 140 150 150 158 C160 150 180 148 192 162 C176 166 162 166 150 162 C138 166 124 166 108 162 Z",
                   C565(0xF4F3EEu), ART_SHADE_HAIR, TOON_MEDIUM);
    } else if (a->id[0] == 'f' && a->id[1] == 'i' && a->id[2] == 'r') {   /* firefighter */
        toon_shape("M118 166 C128 156 144 158 150 164 C156 158 172 156 182 166 C170 172 158 170 150 166 C142 170 130 172 118 166 Z",
                   C565(0x7A3D22u), ART_SHADE_MOUSTACHE, TOON_FINE);
    } else if (a->id[0] == 'c' && a->id[1] == 'a') {                      /* cat */
        toon_line("M120 148 L70 140", 2.5f);
        toon_line("M120 158 L72 162", 2.5f);
        toon_line("M180 148 L230 140", 2.5f);
        toon_line("M180 158 L228 162", 2.5f);
    }
}

/* --------------------------------------------------------------------------- the frame */
#define FACE_VOWEL_FRAMES 24
typedef struct {
    uint8_t vowel;                     /* 0 .. 23 */
    uint8_t loud;                      /* 0 .. 4 */
    uint8_t blink;
} face_pose_t;

/* the character's region (art: rx, ry, rw, rh) for this pose, into buf (rw x rh) */
static void face_draw(const art_char_t *a, face_pose_t p, uint16_t *buf)
{
    float v = (float)p.vowel / (float)(FACE_VOWEL_FRAMES - 1), l = (float)p.loud * 0.25f;
    face_scene(a, buf, a->rx, a->ry, a->rw, a->rh);
    face_patch(a, (uint32_t)(p.blink ? 5u : 0u) + p.loud, buf, a->rx, a->ry, a->rw, a->rh);
    rs_target(buf, a->rx, a->ry, a->rw, a->rh);
    toon_mouth(a, v, 1.0f + l * 0.35f);
    toon_over(a);
}

/* the iOS idle animation (upstream's monk_view.h): a hold with two blinks, then a shuffle of vowels */
typedef struct {
    uint8_t shuffle;
    uint8_t tick, pos;
} face_idle_t;
static const uint8_t IDLE_SEQ[24] = {5, 3, 4, 3, 2, 1, 0, 1, 5, 3, 4, 3, 5, 1, 0, 1, 2, 3, 4, 3, 5, 1, 0, 1};

static void face_idle_reset(face_idle_t *s)
{
    s->shuffle = 0;
    s->tick = 0;
    s->pos = 0;
}

static void face_idle_advance(face_idle_t *s)   /* every 100 ms */
{
    s->tick++;
    if (!s->shuffle) {
        if (s->tick >= 48u) {
            s->shuffle = 1;
            s->tick = 0;
            s->pos = 0;
        }
    } else if (s->tick >= 2u) {
        s->tick = 0;
        if (++s->pos >= 24u) {
            s->shuffle = 0;
            s->pos = 0;
        }
    }
}

/* the idle pose's vowel (0..1) and blink */
static float face_idle_vowel(const face_idle_t *s) { return s->shuffle ? (float)IDLE_SEQ[s->pos] / 5.0f : 1.0f; }
static int face_idle_blink(const face_idle_t *s)
{
    return !s->shuffle && ((s->tick >= 14u && s->tick < 17u) || (s->tick >= 31u && s->tick < 34u));
}
