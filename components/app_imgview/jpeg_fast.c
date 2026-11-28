// A baseline JPEG decoder that decodes only what a view needs. Every block's coefficients have to be read to find where the next one starts, but
// the ones outside the wanted part are skipped without being turned into pixels, and for a one eighth view only the average of each block is used.
// That is what makes a 24 megapixel camera file open in a couple of seconds, a plain decoder spends many seconds turning every block into pixels.
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "jpeg_fast.h"

typedef struct { int id, h, v, tq, dct, act, bw; uint8_t *pad; } comp_t;
typedef struct { uint16_t lut[512]; int maxcode[18], mincode[18], valptr[18]; uint8_t val[256]; bool ok; } huff_t;
struct jf {
    FILE *f;
    long data_off;
    int w, h, nc, hmax, vmax, mcux, mcuy, ri;
    comp_t c[3];
    huff_t dc[4], ac[4];
    uint16_t q[4][64];
    // bit reader
    uint8_t buf[8192];
    int bn, bp;
    uint32_t acc; int cnt;
    bool eod;       // a marker or the end of the file was reached
    int marker;     // the marker that stopped the data, 0 for none
};

static const uint8_t ZZ[64] = {0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,12,19,26,33,40,48,41,34,27,20,13,6,7,14,21,28,35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63};

static void huff_build(huff_t *t, const uint8_t *bits /* 16 */, const uint8_t *vals, int total) {
    memset(t->lut, 0, sizeof t->lut);
    int code = 0, k = 0;
    for (int l = 1; l <= 16; l++) {
        t->valptr[l] = k; t->mincode[l] = code;
        for (int i = 0; i < bits[l - 1]; i++, code++, k++) {
            if (l <= 9) {   // codes up to nine bits are found with one table lookup
                int first = code << (9 - l), n = 1 << (9 - l);
                for (int j = 0; j < n; j++) t->lut[first + j] = (uint16_t)((l << 8) | vals[k]);
            }
        }
        t->maxcode[l] = bits[l - 1] ? code - 1 : -1;
        code <<= 1;
    }
    memcpy(t->val, vals, (size_t)total);
    t->ok = true;
}

static void fill_more(jf_t *j) {   // keeps what is left of the buffer and reads on after it
    int rem = j->bn - j->bp;
    if (rem > 0) memmove(j->buf, j->buf + j->bp, (size_t)rem);
    int n = (int)fread(j->buf + rem, 1, sizeof j->buf - (size_t)rem, j->f);
    j->bn = rem + (n > 0 ? n : 0);
    j->bp = 0;
}
static inline void refill(jf_t *j) {
    while (j->cnt <= 24) {
        int b = 0;
        if (!j->eod) {
            if (j->bn - j->bp < 2) { fill_more(j); if (j->bn - j->bp < 1) j->eod = true; }
            if (!j->eod) {
                b = j->buf[j->bp++];
                if (b == 0xFF) {
                    int nx = j->bp < j->bn ? j->buf[j->bp] : 0xD9;
                    if (nx == 0) j->bp++;                       // a stuffed byte, the data really is FF
                    else { j->eod = true; j->marker = nx; b = 0; j->bp--; }   // a marker ends the data, bp stays on its FF
                }
            }
        }
        j->acc |= (uint32_t)b << (24 - j->cnt);
        j->cnt += 8;
    }
}
static inline int huff_decode(jf_t *j, const huff_t *t) {
    refill(j);
    uint16_t e = t->lut[j->acc >> 23];
    if (e) { int l = e >> 8; j->acc <<= l; j->cnt -= l; return e & 255; }
    int code = (int)(j->acc >> 22) & 0x3FF;   // ten bits to start with, then one more at a time
    int l = 10;
    for (;; l++) {
        if (l > 16) { j->acc <<= 16; j->cnt -= 16; return 0; }
        code = (int)(j->acc >> (32 - l));
        if (t->maxcode[l] >= 0 && code <= t->maxcode[l] && code >= t->mincode[l]) break;
    }
    j->acc <<= l; j->cnt -= l;
    return t->val[t->valptr[l] + code - t->mincode[l]];
}
static inline int get_bits(jf_t *j, int n) {
    if (n == 0) return 0;
    refill(j);
    int v = (int)(j->acc >> (32 - n));
    j->acc <<= n; j->cnt -= n;
    return v;
}
static inline void skip_bits(jf_t *j, int n) {
    while (n > 16) { refill(j); j->acc <<= 16; j->cnt -= 16; n -= 16; }
    refill(j); j->acc <<= n; j->cnt -= n;
}
static inline int extend(int v, int s) { return v < (1 << (s - 1)) ? v - (1 << s) + 1 : v; }

static void restart_sync(jf_t *j) {   // at a restart: throw away the leftover bits, step over the marker, start fresh
    j->acc = 0; j->cnt = 0;
    if (j->marker >= 0xD0 && j->marker <= 0xD7) {   // the reader stopped right at it
        j->bp += 2; j->eod = false; j->marker = 0;
        return;
    }
    for (;;) {   // not found yet: look for it
        if (j->bn - j->bp < 2) { fill_more(j); if (j->bn - j->bp < 2) { j->eod = true; return; } }
        if (j->buf[j->bp] == 0xFF) {
            int nx = j->buf[j->bp + 1];
            if (nx >= 0xD0 && nx <= 0xD7) { j->bp += 2; j->eod = false; j->marker = 0; return; }
            if (nx != 0 && nx != 0xFF) { j->eod = true; j->marker = nx; return; }
        }
        j->bp++;
    }
}

jf_t *jf_open(FILE *f, int *w, int *h) {
    jf_t *j = heap_caps_calloc(1, sizeof *j, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!j) return NULL;
    j->f = f;
    rewind(f);
    uint8_t b2[2];
    if (fread(b2, 1, 2, f) != 2 || b2[0] != 0xFF || b2[1] != 0xD8) goto bad;
    bool got_sof = false;
    for (;;) {
        uint8_t m[4];
        int c;
        do { c = fgetc(f); } while (c != 0xFF && c != EOF);
        do { c = fgetc(f); } while (c == 0xFF);
        if (c == EOF) goto bad;
        int mk = c;
        if (mk == 0xD8 || mk == 0x01 || (mk >= 0xD0 && mk <= 0xD7) || mk == 0) continue;
        if (fread(m, 1, 2, f) != 2) goto bad;
        int len = (m[0] << 8) | m[1];
        if (len < 2) goto bad;
        uint8_t *seg = malloc((size_t)len);
        if (!seg) goto bad;
        if (fread(seg, 1, (size_t)len - 2, f) != (size_t)len - 2) { free(seg); goto bad; }
        int sl = len - 2;
        if (mk == 0xC0 || mk == 0xC1) {   // baseline, or extended sequential with 8 bit samples
            if (sl < 6 || seg[0] != 8) { free(seg); goto bad; }
            j->h = (seg[1] << 8) | seg[2]; j->w = (seg[3] << 8) | seg[4]; j->nc = seg[5];
            if ((j->nc != 1 && j->nc != 3) || sl < 6 + 3 * j->nc || j->w < 1 || j->h < 1) { free(seg); goto bad; }
            for (int i = 0; i < j->nc; i++) {
                j->c[i].id = seg[6 + 3 * i]; j->c[i].h = seg[7 + 3 * i] >> 4; j->c[i].v = seg[7 + 3 * i] & 15; j->c[i].tq = seg[8 + 3 * i] & 3;
                if (j->nc == 1) j->c[i].h = j->c[i].v = 1;
                if (j->c[i].h < 1 || j->c[i].h > 2 || j->c[i].v < 1 || j->c[i].v > 2) { free(seg); goto bad; }
                if (j->c[i].h > j->hmax) j->hmax = j->c[i].h;
                if (j->c[i].v > j->vmax) j->vmax = j->c[i].v;
            }
            j->mcux = (j->w + 8 * j->hmax - 1) / (8 * j->hmax); j->mcuy = (j->h + 8 * j->vmax - 1) / (8 * j->vmax);
            got_sof = true;
        } else if (mk == 0xC2 || mk == 0xC3 || (mk >= 0xC5 && mk <= 0xCF && mk != 0xC8 && mk != 0xCC && mk != 0xC4)) { free(seg); goto bad; }   // progressive, lossless, arithmetic: not here
        else if (mk == 0xDB) {
            for (int o = 0; o < sl;) {
                int pq = seg[o] >> 4, tq = seg[o] & 3;
                o++;
                if (o + (pq ? 128 : 64) > sl) break;
                for (int i = 0; i < 64; i++) j->q[tq][ZZ[i]] = pq ? (uint16_t)((seg[o + 2 * i] << 8) | seg[o + 2 * i + 1]) : seg[o + i];   // kept in natural order
                o += pq ? 128 : 64;
            }
        } else if (mk == 0xC4) {
            for (int o = 0; o + 17 <= sl;) {
                int tc = seg[o] >> 4, th = seg[o] & 3;
                int total = 0;
                for (int l = 0; l < 16; l++) total += seg[o + 1 + l];
                if (total > 256 || o + 17 + total > sl) break;
                huff_build(tc == 0 ? &j->dc[th] : &j->ac[th], seg + o + 1, seg + o + 17, total);
                o += 17 + total;
            }
        } else if (mk == 0xDD) { if (sl >= 2) j->ri = (seg[0] << 8) | seg[1]; }
        else if (mk == 0xDA) {   // the one scan of a baseline picture
            if (!got_sof || sl < 1 + 2 * seg[0] + 3 || seg[0] != j->nc) { free(seg); goto bad; }
            for (int i = 0; i < j->nc; i++) {
                int sel = -1;
                for (int k = 0; k < j->nc; k++) if (j->c[k].id == seg[1 + 2 * i]) sel = k;
                if (sel < 0) { free(seg); goto bad; }
                j->c[sel].dct = seg[2 + 2 * i] >> 4; j->c[sel].act = seg[2 + 2 * i] & 3;
            }
            j->data_off = ftell(f);
            free(seg);
            *w = j->w; *h = j->h;
            return j;
        }
        free(seg);
    }
bad:
    free(j);
    return NULL;
}
void jf_close(jf_t *j) { free(j); }

// ---- the transform. A view shrunk by 1, 2, 4 or 8 only needs the lowest n by n frequencies of each block, n = 8, 4, 2 or 1, and an n point
// inverse transform of them gives the block shrunk to n by n pixels. The scale is n/8 for the two directions together.
static float CS[9][8][8];   // CS[n][x][u]: the n point inverse transform
static bool cos_ready;
static void cos_init(void) {
    for (int n = 1; n <= 8; n *= 2) for (int x = 0; x < n; x++) for (int u = 0; u < n; u++)
        CS[n][x][u] = (float)((u == 0 ? sqrt(1.0 / n) : sqrt(2.0 / n)) * cos((2 * x + 1) * u * 3.14159265358979 / (2.0 * n)));
    cos_ready = true;
}
// coef: the dequantized block in natural order (8 wide). out: n by n samples, 0 to 255.
static void idct_n(const int16_t *coef, int n, uint8_t *out) {
    float scale = (float)n / 8.0f;
    if (n == 1) { int p = (int)lrintf(coef[0] * scale) + 128; out[0] = (uint8_t)(p < 0 ? 0 : p > 255 ? 255 : p); return; }
    float t[64];
    for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) {
        float s = 0;
        for (int u = 0; u < n; u++) s += CS[n][x][u] * coef[y * 8 + u];
        t[y * 8 + x] = s;
    }
    for (int x = 0; x < n; x++) for (int y = 0; y < n; y++) {
        float s = 0;
        for (int v = 0; v < n; v++) s += CS[n][y][v] * t[v * 8 + x];
        int p = (int)lrintf(s * scale) + 128;
        out[y * n + x] = (uint8_t)(p < 0 ? 0 : p > 255 ? 255 : p);
    }
}
static inline uint16_t pack565(int r, int g, int b) {
    r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b;
    uint16_t v = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    return (uint16_t)((v >> 8) | (v << 8));
}

bool jf_region(jf_t *j, int den, int x0, int y0, int rw, int rh, uint16_t *out) {
    if (!cos_ready) cos_init();
    if (den != 1 && den != 2 && den != 4 && den != 8) return false;
    int n = 8 / den;                                   // the block size after shrinking
    int mw = 8 * j->hmax, mh = 8 * j->vmax;            // an MCU in pixels
    int ow = rw / den, oh = rh / den;                  // the output size
    int mow = mw / den, moh = mh / den;                // an MCU in output pixels
    memset(out, 0, (size_t)ow * oh * 2);
    if (fseek(j->f, j->data_off, SEEK_SET) != 0) return false;
    j->bn = j->bp = 0; j->acc = 0; j->cnt = 0; j->eod = false; j->marker = 0;
    int pred[3] = {0, 0, 0};
    int mx0 = x0 / mw, mx1 = (x0 + rw - 1) / mw, my0 = y0 / mh, my1 = (y0 + rh - 1) / mh;
    if (mx1 >= j->mcux) mx1 = j->mcux - 1;
    if (my1 >= j->mcuy) my1 = j->mcuy - 1;
    int16_t coef[64];
    uint8_t blk[3][4][64];                             // the blocks of one MCU, n by n samples each, by component
    int mcu = 0;
    for (int my = 0; my <= my1; my++) {
        for (int mx = 0; mx < j->mcux; mx++, mcu++) {
            if (j->ri && mcu && mcu % j->ri == 0) { restart_sync(j); pred[0] = pred[1] = pred[2] = 0; }
            bool inside = mx >= mx0 && mx <= mx1 && my >= my0;
            for (int ci = 0; ci < j->nc; ci++) {
                comp_t *c = &j->c[ci];
                const huff_t *td = &j->dc[c->dct], *ta = &j->ac[c->act];
                for (int bi = 0; bi < c->h * c->v; bi++) {
                    int s = huff_decode(j, td);
                    int diff = s ? extend(get_bits(j, s), s) : 0;
                    pred[ci] += diff;
                    if (!inside || n == 1) {   // the rest of this block is only skipped
                        int k = 1;
                        while (k < 64) {
                            int sym = huff_decode(j, ta);
                            int r = sym >> 4, sz = sym & 15;
                            if (!sz) { if (r == 15) { k += 16; continue; } break; }
                            k += r + 1;
                            skip_bits(j, sz);
                        }
                        if (inside) { int p = ((pred[ci] * j->q[c->tq][0] + 4) >> 3) + 128; blk[ci][bi][0] = (uint8_t)(p < 0 ? 0 : p > 255 ? 255 : p); }
                        continue;
                    }
                    memset(coef, 0, sizeof coef);
                    coef[0] = (int16_t)(pred[ci] * j->q[c->tq][0]);
                    int k = 1;
                    while (k < 64) {
                        int sym = huff_decode(j, ta);
                        int r = sym >> 4, sz = sym & 15;
                        if (!sz) { if (r == 15) { k += 16; continue; } break; }
                        k += r;
                        if (k > 63) break;
                        int v = extend(get_bits(j, sz), sz);
                        int nat = ZZ[k];
                        if ((nat & 7) < n && (nat >> 3) < n) coef[nat] = (int16_t)(v * j->q[c->tq][nat]);   // only the low frequencies are used
                        k++;
                    }
                    idct_n(coef, n, blk[ci][bi]);
                }
            }
            if (!inside) continue;
            // the pixels of this MCU, one for each sample of the shrunk blocks
            for (int py = 0; py < moh; py++) for (int px = 0; px < mow; px++) {
                int ox = mx * mow + px - x0 / den, oy = my * moh + py - y0 / den;
                if (ox < 0 || oy < 0 || ox >= ow || oy >= oh || mx * mw + px * den >= j->w || my * mh + py * den >= j->h) continue;
                int yb = (py / n) * j->c[0].h + (px / n);
                int Y = blk[0][yb][(py % n) * n + (px % n)];
                int r, g, b;
                if (j->nc == 1) r = g = b = Y;
                else {
                    int csx = px * j->c[1].h / j->hmax, csy = py * j->c[1].v / j->vmax;
                    int cbi = (csy / n) * j->c[1].h + (csx / n), cri = (csy / n) * j->c[2].h + (csx / n);
                    int Cb = (int)blk[1][cbi][(csy % n) * n + (csx % n)] - 128, Cr = (int)blk[2][cri][(csy % n) * n + (csx % n)] - 128;
                    r = Y + ((91881 * Cr) >> 16); g = Y - ((22554 * Cb + 46802 * Cr) >> 16); b = Y + ((116130 * Cb) >> 16);
                }
                out[(size_t)oy * ow + ox] = pack565(r, g, b);
            }
        }
    }
    return true;
}
