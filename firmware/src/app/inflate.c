/* SPDX-License-Identifier: GPL-3.0-only */
/* From fm1-radio (app/inflate.c), unchanged but for this note: here it unpacks the characters' scenes and faces
 * (tools/gen_art.py: raw deflate, a 1 KB window), each band read in place from the firmware image. */
/* Raw deflate (RFC 1951), streaming, for the places table's blocks (rg_places.py: zlib wbits -10, a 1 KB
 * window), read in place from the firmware image; each byte goes to out(), which may stop it. About 2.4 KB
 * of stack, no heap. Decoding as tinf (Joergen Ibsen) does: canonical Huffman walked a bit at a time. Part
 * of the unity build (main_wifi.c). */
#define INF_WIN 1024u

typedef struct { uint16_t counts[16], symbols[288]; } inf_tree_t;
typedef struct {
    const uint8_t *p, *end;                       /* the next byte to read, the block's end */
    uint32_t bits, nbits;
    uint8_t win[INF_WIN];
    uint32_t wpos;
    int (*out)(void *ctx, uint8_t c);             /* nonzero: stop */
    void *ctx;
    int err;
    inf_tree_t lt, dt;
} inf_t;

static uint32_t inf_byte(inf_t *d)
{
    if (d->p == d->end) {
        d->err = 1;                               /* ran off the block */
        return 0;
    }
    return *d->p++;
}
static uint32_t inf_bits(inf_t *d, uint32_t n)    /* n bits, least significant first */
{
    uint32_t v;
    while (d->nbits < n) {
        d->bits |= inf_byte(d) << d->nbits;
        d->nbits += 8u;
    }
    v = d->bits & ((1u << n) - 1u);
    d->bits >>= n;
    d->nbits -= n;
    return v;
}

static void inf_build(inf_tree_t *t, const uint8_t *len, uint32_t n)
{
    uint16_t offs[16];
    uint32_t i, sum = 0;
    memset(t->counts, 0, sizeof t->counts);
    for (i = 0; i < n; i++)
        t->counts[len[i]]++;
    t->counts[0] = 0;
    for (i = 0; i < 16u; i++) {
        offs[i] = (uint16_t)sum;
        sum += t->counts[i];
    }
    for (i = 0; i < n; i++)
        if (len[i])
            t->symbols[offs[len[i]]++] = (uint16_t)i;
}
static uint32_t inf_sym(inf_t *d, const inf_tree_t *t)
{
    int32_t cur = 0, sum = 0, len = 0;
    do {
        cur = 2 * cur + (int32_t)inf_bits(d, 1);
        if (++len > 15) {
            d->err = 1;
            return 256;
        }
        sum += t->counts[len];
        cur -= t->counts[len];
    } while (cur >= 0);
    return t->symbols[sum + cur];
}

static int inf_emit(inf_t *d, uint8_t c)
{
    d->win[d->wpos++ & (INF_WIN - 1u)] = c;
    return d->out(d->ctx, c);
}

static int inf_codes(inf_t *d)                    /* a compressed block's data; 1 = stopped by out() */
{
    static const uint16_t LBASE[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
                                       67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const uint8_t LEXT[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4,
                                     5, 5, 5, 5, 0};
    static const uint16_t DBASE[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
                                       513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const uint8_t DEXT[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10,
                                     11, 11, 12, 12, 13, 13};
    for (;;) {
        uint32_t sym = inf_sym(d, &d->lt), len, dist, ds;
        if (d->err)
            return -1;
        if (sym < 256u) {
            if (inf_emit(d, (uint8_t)sym))
                return 1;
            continue;
        }
        if (sym == 256u)
            return 0;
        sym -= 257u;
        if (sym >= 29u)
            return -1;
        len = LBASE[sym] + inf_bits(d, LEXT[sym]);
        ds = inf_sym(d, &d->dt);
        if (ds >= 30u)
            return -1;
        dist = DBASE[ds] + inf_bits(d, DEXT[ds]);
        if (dist > INF_WIN || dist > d->wpos)
            return -1;
        while (len--)
            if (inf_emit(d, d->win[(d->wpos - dist) & (INF_WIN - 1u)]))
                return 1;
    }
}

static int inf_dynamic(inf_t *d)                  /* a block's own code tables */
{
    static const uint8_t ORDER[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    uint8_t len[288 + 32];
    uint32_t hlit = inf_bits(d, 5) + 257u, hdist = inf_bits(d, 5) + 1u, hclen = inf_bits(d, 4) + 4u, i, n;
    memset(len, 0, 19);
    for (i = 0; i < hclen; i++)
        len[ORDER[i]] = (uint8_t)inf_bits(d, 3);
    inf_build(&d->lt, len, 19);                   /* the code-length code, in lt for a moment */
    for (n = 0; n < hlit + hdist;) {
        uint32_t sym = inf_sym(d, &d->lt), rep, v = 0;
        if (d->err || sym > 18u)
            return -1;
        if (sym < 16u) {
            len[n++] = (uint8_t)sym;
            continue;
        }
        if (sym == 16u) {
            if (!n)
                return -1;
            v = len[n - 1u];
            rep = 3u + inf_bits(d, 2);
        } else if (sym == 17u) {
            rep = 3u + inf_bits(d, 3);
        } else {
            rep = 11u + inf_bits(d, 7);
        }
        if (n + rep > hlit + hdist)
            return -1;
        while (rep--)
            len[n++] = (uint8_t)v;
    }
    inf_build(&d->lt, len, hlit);
    inf_build(&d->dt, len + hlit, hdist);
    return 0;
}

/* inflate [src, src + n) into out(); 0 = done or stopped, < 0 = bad data */
static int inflate_mem(const uint8_t *src, uint32_t n, int (*out)(void *, uint8_t), void *ctx)
{
    inf_t d;
    uint32_t final;
    memset(&d, 0, sizeof d);
    d.p = src;
    d.end = src + n;
    d.wpos = 0;
    d.out = out;
    d.ctx = ctx;
    d.err = 0;
    do {
        uint32_t type;
        int rc;
        final = inf_bits(&d, 1);
        type = inf_bits(&d, 2);
        if (d.err)
            return -1;
        if (type == 0) {                          /* stored */
            uint32_t len;
            d.bits = 0;
            d.nbits = 0;
            len = inf_byte(&d) | inf_byte(&d) << 8;
            (void)inf_byte(&d);
            (void)inf_byte(&d);
            while (len--) {
                uint8_t c = (uint8_t)inf_byte(&d);
                if (d.err)
                    return -1;
                if (inf_emit(&d, c))
                    return 0;
            }
            continue;
        }
        if (type == 1) {                          /* the fixed codes */
            uint8_t len[288];
            uint32_t i;
            for (i = 0; i < 288u; i++)
                len[i] = i < 144u ? 8 : i < 256u ? 9 : i < 280u ? 7 : 8;
            inf_build(&d.lt, len, 288);
            for (i = 0; i < 30u; i++)
                len[i] = 5;
            inf_build(&d.dt, len, 30);
        } else if (type != 2 || inf_dynamic(&d)) {
            return -1;
        }
        rc = inf_codes(&d);
        if (rc)
            return rc > 0 ? 0 : -1;
    } while (!final);
    return 0;
}
