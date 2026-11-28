// The first scan inside a progressive JPEG already is a picture, one eighth of the size in each direction. This reads just that scan, and the
// extra bits that sharpen the DC values, and makes the small picture from it. It needs very little memory, so a huge progressive JPEG opens.
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "jpeg_dc.h"

typedef struct { uint8_t bits[17]; uint8_t val[256]; int mincode[17], maxcode[17], valptr[17]; bool ok; } huff_t;
typedef struct { int id, h, v, tq; int pw, ph; int16_t *plane; int pred; int dctab; } comp_t;
typedef struct {
    const uint8_t *d; size_t n, p;
    uint32_t bitbuf; int bitcnt; bool hit_marker;
    int w, h, nc, hmax, vmax, mcux, mcuy, ri;
    comp_t c[3];
    huff_t dc[4];
    uint16_t q[4][64];
    char err[64];
} jd_t;

static void huff_build(huff_t *t) {
    int code = 0, k = 0;
    for (int l = 1; l <= 16; l++) {
        t->valptr[l] = k; t->mincode[l] = code;
        code += t->bits[l]; k += t->bits[l];
        t->maxcode[l] = t->bits[l] ? code - 1 : -1;
        code <<= 1;
    }
    t->ok = true;
}
static int getbit(jd_t *j) {
    if (j->bitcnt == 0) {
        int b = 0;
        if (!j->hit_marker && j->p < j->n) {
            b = j->d[j->p];
            if (b == 0xFF) {
                int nx = j->p + 1 < j->n ? j->d[j->p + 1] : 0xD9;
                if (nx == 0) j->p += 2;
                else { j->hit_marker = true; b = 0; }   // a marker ends the data, zeros after it
            } else j->p++;
        } else j->hit_marker = true;
        j->bitbuf = (uint32_t)b; j->bitcnt = 8;
    }
    j->bitcnt--;
    return (j->bitbuf >> j->bitcnt) & 1;
}
static int getbits(jd_t *j, int n) { int v = 0; while (n--) v = (v << 1) | getbit(j); return v; }
static int huff_decode(jd_t *j, const huff_t *t) {
    int code = 0;
    for (int l = 1; l <= 16; l++) {
        code = (code << 1) | getbit(j);
        if (t->maxcode[l] >= 0 && code <= t->maxcode[l] && code >= t->mincode[l]) return t->val[t->valptr[l] + code - t->mincode[l]];
    }
    return 0;
}
static void restart(jd_t *j) {   // after a restart interval: drop the leftover bits, find the RSTn marker and step over it
    j->bitcnt = 0; j->hit_marker = false;
    while (j->p + 1 < j->n && !(j->d[j->p] == 0xFF && j->d[j->p + 1] >= 0xD0 && j->d[j->p + 1] <= 0xD7)) {
        if (j->d[j->p] == 0xFF && j->d[j->p + 1] != 0 && j->d[j->p + 1] != 0xFF) return;   // some other marker: the scan is over
        j->p++;
    }
    if (j->p + 1 < j->n) j->p += 2;
}

static void do_scan(jd_t *j, int ns, int *sel, int al, bool refine) {
    for (int i = 0; i < ns; i++) j->c[sel[i]].pred = 0;
    j->bitcnt = 0; j->hit_marker = false;
    int mcu = 0;
    if (ns > 1) {
        for (int my = 0; my < j->mcuy; my++) for (int mx = 0; mx < j->mcux; mx++) {
            if (j->ri && mcu && mcu % j->ri == 0) { restart(j); for (int i = 0; i < ns; i++) j->c[sel[i]].pred = 0; }
            mcu++;
            for (int i = 0; i < ns; i++) {
                comp_t *c = &j->c[sel[i]];
                for (int v = 0; v < c->v; v++) for (int h = 0; h < c->h; h++) {
                    int16_t *dst = &c->plane[(size_t)(my * c->v + v) * c->pw + mx * c->h + h];
                    if (refine) { if (getbit(j)) *dst = (int16_t)(*dst | (1 << al)); }
                    else {
                        int s = huff_decode(j, &j->dc[c->dctab]);
                        int diff = 0;
                        if (s > 0 && s < 16) { diff = getbits(j, s); if (diff < (1 << (s - 1))) diff -= (1 << s) - 1; }
                        c->pred += diff;
                        *dst = (int16_t)(c->pred * (1 << al));
                    }
                }
            }
        }
    } else {
        comp_t *c = &j->c[sel[0]];
        int cw = (j->w * c->h + j->hmax - 1) / j->hmax, ch = (j->h * c->v + j->vmax - 1) / j->vmax;
        int bw = (cw + 7) / 8, bh = (ch + 7) / 8;
        if (j->nc == 1) { bw = (j->w + 7) / 8; bh = (j->h + 7) / 8; }
        for (int by = 0; by < bh; by++) for (int bx = 0; bx < bw; bx++) {
            if (j->ri && mcu && mcu % j->ri == 0) { restart(j); c->pred = 0; }
            mcu++;
            int16_t *dst = &c->plane[(size_t)by * c->pw + bx];
            if (refine) { if (getbit(j)) *dst = (int16_t)(*dst | (1 << al)); }
            else {
                int s = huff_decode(j, &j->dc[c->dctab]);
                int diff = 0;
                if (s > 0 && s < 16) { diff = getbits(j, s); if (diff < (1 << (s - 1))) diff -= (1 << s) - 1; }
                c->pred += diff;
                *dst = (int16_t)(c->pred * (1 << al));
            }
        }
    }
}
static inline uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

uint16_t *jpegdc_decode(const uint8_t *d, size_t n, int *ow, int *oh, int *full_w, int *full_h, char *err, size_t errcap) {
    jd_t *j = heap_caps_calloc(1, sizeof *j, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint16_t *out = NULL;
    if (!j) { snprintf(err, errcap, "Not enough memory for that picture."); return NULL; }
    j->d = d; j->n = n; j->p = 2;
    bool got_sof = false, got_dc = false;
    while (j->p + 4 <= n) {
        if (d[j->p] != 0xFF) { j->p++; continue; }
        int m = d[j->p + 1];
        if (m == 0xFF) { j->p++; continue; }
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7) || m == 0x00) { j->p += 2; continue; }
        if (m == 0xD9) break;
        size_t len = ((size_t)d[j->p + 2] << 8) | d[j->p + 3];
        const uint8_t *s = d + j->p + 4;
        size_t sl = len >= 2 ? len - 2 : 0;
        if (j->p + 2 + len > n) break;
        if (m == 0xC2) {   // progressive frame
            if (sl < 6 || s[0] != 8) { snprintf(err, errcap, "That JPEG type is not supported."); goto bad; }
            j->h = (s[1] << 8) | s[2]; j->w = (s[3] << 8) | s[4]; j->nc = s[5];
            if ((j->nc != 1 && j->nc != 3) || sl < (size_t)(6 + 3 * j->nc) || j->w < 1 || j->h < 1) { snprintf(err, errcap, "That JPEG type is not supported."); goto bad; }
            for (int i = 0; i < j->nc; i++) {
                j->c[i].id = s[6 + 3 * i]; j->c[i].h = s[7 + 3 * i] >> 4; j->c[i].v = s[7 + 3 * i] & 15; j->c[i].tq = s[8 + 3 * i] & 3;
                if (j->c[i].h < 1 || j->c[i].h > 4 || j->c[i].v < 1 || j->c[i].v > 4) { snprintf(err, errcap, "That JPEG type is not supported."); goto bad; }
                if (j->nc == 1) j->c[i].h = j->c[i].v = 1;
                if (j->c[i].h > j->hmax) j->hmax = j->c[i].h;
                if (j->c[i].v > j->vmax) j->vmax = j->c[i].v;
            }
            j->mcux = (j->w + 8 * j->hmax - 1) / (8 * j->hmax); j->mcuy = (j->h + 8 * j->vmax - 1) / (8 * j->vmax);
            for (int i = 0; i < j->nc; i++) {
                j->c[i].pw = j->mcux * j->c[i].h; j->c[i].ph = j->mcuy * j->c[i].v;
                j->c[i].plane = heap_caps_calloc((size_t)j->c[i].pw * j->c[i].ph, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!j->c[i].plane) { snprintf(err, errcap, "Not enough memory for that picture."); goto bad; }
            }
            got_sof = true;
        } else if (m == 0xDB) {   // quantization tables, only the first value of each matters here
            for (size_t o = 0; o < sl;) {
                int pq = s[o] >> 4, tq = s[o] & 3;
                o++;
                if (o + (pq ? 128 : 64) > sl) break;
                j->q[tq][0] = pq ? (uint16_t)((s[o] << 8) | s[o + 1]) : s[o];
                o += pq ? 128 : 64;
            }
        } else if (m == 0xC4) {   // Huffman tables
            for (size_t o = 0; o + 17 <= sl;) {
                int tc = s[o] >> 4, th = s[o] & 15;
                int total = 0;
                huff_t tmp; memset(&tmp, 0, sizeof tmp);
                for (int l = 1; l <= 16; l++) { tmp.bits[l] = s[o + l]; total += s[o + l]; }
                if (total > 256 || o + 17 + (size_t)total > sl) break;
                memcpy(tmp.val, s + o + 17, (size_t)total);
                huff_build(&tmp);
                if (tc == 0 && th < 4) j->dc[th] = tmp;
                o += 17 + (size_t)total;
            }
        } else if (m == 0xDD) { if (sl >= 2) j->ri = (s[0] << 8) | s[1]; }
        else if (m == 0xDA) {   // a scan: only the DC ones are read, the data after the header is skipped to the next marker either way
            if (!got_sof || sl < 1) { snprintf(err, errcap, "That JPEG could not be read."); goto bad; }
            int ns = s[0];
            if (sl < (size_t)(1 + 2 * ns + 3) || ns < 1 || ns > 3) { snprintf(err, errcap, "That JPEG could not be read."); goto bad; }
            int sel[3];
            for (int i = 0; i < ns; i++) {
                sel[i] = -1;
                for (int k = 0; k < j->nc; k++) if (j->c[k].id == s[1 + 2 * i]) sel[i] = k;
                if (sel[i] < 0) { snprintf(err, errcap, "That JPEG could not be read."); goto bad; }
                j->c[sel[i]].dctab = (s[2 + 2 * i] >> 4) & 3;
            }
            int ss = s[1 + 2 * ns], se = s[2 + 2 * ns], ah = s[3 + 2 * ns] >> 4, al = s[3 + 2 * ns] & 15;
            j->p += 2 + len;
            if (ss == 0 && se == 0 && (ah == 0 || got_dc)) {
                do_scan(j, ns, sel, al, ah != 0);
                if (ah == 0) got_dc = true;
            }
            // on to the next marker
            while (j->p + 1 < n && !(d[j->p] == 0xFF && d[j->p + 1] != 0 && !(d[j->p + 1] >= 0xD0 && d[j->p + 1] <= 0xD7) && d[j->p + 1] != 0xFF)) j->p++;
            continue;
        }
        j->p += 2 + len;
    }
    if (!got_sof || !got_dc) { snprintf(err, errcap, "That JPEG could not be read."); goto bad; }
    {
        int ocw = (j->w + 7) / 8, och = (j->h + 7) / 8;
        out = heap_caps_malloc((size_t)ocw * och * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!out) { snprintf(err, errcap, "Not enough memory for that picture."); goto bad; }
        for (int by = 0; by < och; by++) for (int bx = 0; bx < ocw; bx++) {
            int val[3];
            for (int i = 0; i < j->nc; i++) {
                comp_t *c = &j->c[i];
                int cbx = bx * c->h / j->hmax, cby = by * c->v / j->vmax;
                if (cbx >= c->pw) cbx = c->pw - 1;
                if (cby >= c->ph) cby = c->ph - 1;
                val[i] = ((int)c->plane[(size_t)cby * c->pw + cbx] * (int)j->q[c->tq][0] + 4) >> 3;   // the average of the 8 by 8 block, around zero
            }
            int r, g, b;
            if (j->nc == 1) r = g = b = val[0] + 128;
            else {
                int Y = val[0] + 128, Cb = val[1], Cr = val[2];
                r = Y + ((91881 * Cr) >> 16); g = Y - ((22554 * Cb + 46802 * Cr) >> 16); b = Y + ((116130 * Cb) >> 16);
            }
            uint16_t v = (uint16_t)(((clamp8(r) & 0xF8) << 8) | ((clamp8(g) & 0xFC) << 3) | (clamp8(b) >> 3));
            out[(size_t)by * ocw + bx] = (uint16_t)((v >> 8) | (v << 8));
        }
        *ow = ocw; *oh = och; *full_w = j->w; *full_h = j->h;
    }
    for (int i = 0; i < 3; i++) free(j->c[i].plane);
    free(j);
    return out;
bad:
    for (int i = 0; i < 3; i++) free(j->c[i].plane);
    free(j); free(out);
    return NULL;
}
