/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Pierre Arnaud GUYOT (OktoPierre) */
/* WAVELET: engine A (the subtractive one) of the Wavelet Synthesizer (github.com/PierreArnaudGUYOT/
 * Free-Wavelet-Synthesizer-VST, GPL-3.0-or-later), reduced for the FM-1 and written in fixed point.
 *
 * The idea: one cycle of a source wave (saw, square, pulse, triangle, sine or a noise loop) is split by a
 * periodic orthogonal discrete wavelet transform into WAV_NB = 9 bands of atoms; band b holds about the
 * harmonics 2^(b-2) .. 2^(b-1) of the cycle (band 1 the fundamental, band 9 harmonics 128..256). SPRS keeps
 * only the largest atoms. Each band is resynthesised on its own into a layer; the layers (and their sum,
 * DRY) are the voice's wave table: 512 samples x 10 layers, 16-bit. A voice reads the table at its pitch
 * and adds the layers with one gain each: the scale filter (low-, high- or band-pass in bands, a slope in
 * dB per band, a resonance peak at the cutoff), so the "filter" acts on the atoms of the wavelet, not on
 * frequencies. The WNDW cross-fades the filtered sum with the DRY cycle inside a raised-cosine window
 * (SHAPE sets where in the cycle: the LFO and the envelope move it).
 *
 * ENGINE B (the additive one) is a mode of this engine: WAVE 6 .. 11 (RAMP MIRR ALT WALS IMPL RAND) set the atoms
 * from a pattern instead of a source wave (no forward transform, no sparsity); each band's layer is peak-normalised,
 * and a voice's gain per band is  registration x tilt x scale envelope x filter / norm: BARS picks one of 16
 * registrations of the nine bands (the VST's drawbars), TILT is the dB per band, the scale envelope swells over the
 * amp ATK and, with FADE, the upper bands fall to half faster than the lower ones. In that mode EDIT 1 / 2 read
 * WAVE WVLT BARS TILT / CUT RES FLT FADE.
 *
 * What the FM-1 version leaves out (the VST / web app has it): wavelet morphing, LFO 2, the sub oscillator, key
 * tracking below 100 %, the filter's velocity amount, the window's width (fixed at half a cycle), engine B's
 * separate scale attack, spread and sustain and its own random seeds, the A/B mix (a sound is A or B). The cutoff
 * is in bands (0 .. 12), so the timbre follows the key (the original's KEY TRK at 100 %).
 *
 * Parameters (EDIT 1: WAVE WVLT PHAS/WDTH SPRS; EDIT 2: CUT RES FLT WNDW):
 *   E0 WAVE   SAW SQR PLS TRI SIN NOIS | engine B: RAMP MIRR ALT WALS IMPL RAND
 *   E1 WVLT   HAAR DB2 DB4 DB8 SYM8 COIF3 DB20 (the wavelet: Daubechies, symlets, coiflets)
 *   E2 PHAS   the source's phase, 0 .. 127 = 0 .. 127/128 of a cycle (the DWT is shift-variant: the same wave
 *             shifted gives other atoms, so another timbre); on PLS and TRI it is WDTH, the pulse width /
 *             the triangle's rise, 2 .. 98 %
 *   E3 SPRS   sparsity: the share of atoms thrown away (the largest ones stay)
 *   E4 CUT    the cutoff, in bands (127 = band 12: fully open); the LFO's and the envelope's FLT amounts
 *             (EDIT > ENV / LFO) move it by up to +-12 bands
 *   E5 RES    resonance, 0 .. 24 dB at the cutoff
 *   E6 FLT    the filter: LP / HP / BP and its slope, 12 18 24 36 48 96 dB per band
 *   E7 WNDW   the window's depth: 0 the filtered sound everywhere; 127 the dry cycle outside the window
 *
 * Tables: a part's engine memory holds its table (wav_part_t); a change of E0 .. E3 rebuilds it. The build
 * runs in the audio interrupt, a bounded slice per block (WAV_QUANTUM multiply-adds), in one shared scratch
 * (wav_scr, the pool): source, forward DWT level by level, sparsify, then each band's inverse DWT, 20 .. 400
 * thousand multiply-adds (Haar .. DB20), so 10 .. 150 ms with no audible cost. wav_tick() (fx.c mix_block,
 * core 0, before the parts render on either core) drives it: it starts a build when a WAVELET part's E0 .. E3
 * differ from its table's, restarts it when they move again, and when it is done the part cross-fades from
 * its table to the new one over WAV_XF blocks, then the new table is copied over the old (in wav_tick: no
 * part renders while it runs). A part with no table yet (power-on, after another engine) is silent until
 * its first build (a few ms) and takes it without a fade (also after a visit of any other engine). Only wav_tick
 * touches wav_scr's state; the renders
 * read wav_scr.t while the part cross-fades.
 *
 * Fixed point: table samples Q14 (the source's peak = 16384); band gains Q10 (1024 = 0 dB, 16384 = +24 dB,
 * the cap); the transforms in Q20 samples with Q28 coefficients (64-bit products); phases uint32 as the other
 * engines. The sum of nine layers x gains at Q24 stays in 32 bits for any real table (the resonance peaks
 * one band; the layers of a peak-normalised cycle stay near 1.0). */

#include "wavelet_tables.h"
#include "wavelet_presets.h"                            /* WAVELET_PRESETS, WAV_REG (tools/gen_wavelet_presets.js) */

#define WAV_N 512u                                  /* samples per cycle */
#define WAV_LOG2N 9u
#define WAV_NB 9u                                   /* bands (layers 0 .. 8), band b = layer b - 1 */
#define WAV_NL 10u                                  /* layers: the bands and DRY */
#define WAV_DRY 9u
#define WAV_TSIZE ((WAV_N + 1u) * WAV_NL)           /* with one guard row (the interpolation's i + 1) */
#define WAV_XF 16u                                  /* cross-fade to a new table: blocks (512 samples) */
#ifndef WAV_QUANTUM
#define WAV_QUANTUM 3000                            /* multiply-adds of the build per audio block */
#endif
#define WAV_GMAX 16384                              /* +24 dB in Q10 */

static const char *const N_WWAVE[] = {"SAW", "SQR", "PLS", "TRI", "SIN", "NOIS", "RAMP", "MIRR", "ALT", "WALS", "IMPL", "RAND"};
static const char *const N_WFLT[] = {"LP12", "LP18", "LP24", "LP36", "LP48", "LP96",
                                     "HP12", "HP18", "HP24", "HP36", "HP48", "HP96",
                                     "BP12", "BP18", "BP24", "BP36", "BP48", "BP96"};
static const uint8_t WAV_SLOPE[6] = {12, 18, 24, 36, 48, 96};
enum { WW_SAW, WW_SQR, WW_PLS, WW_TRI, WW_SIN, WW_NOIS, WW_B0 };   /* WW_B0 and up: engine B (patterns 0 .. 5) */
#define WAV_IS_B(w) ((uint32_t)(w) >= WW_B0)

/* ------------------------------------------------------ the part's state --- */
typedef struct {
    int16_t t[WAV_TSIZE];                           /* the table: row i = the layers at sample i, Q14 */
    int16_t g0[NVOICE][WAV_NB];                     /* each voice's band gains at the end of its last block, Q10 */
    uint16_t lv[NVOICE][WAV_NB];                    /* engine B: each voice's scale envelope per band, Q15 */
    uint8_t atk[NVOICE];                            /* engine B: 1 while the scale envelope swells (the same for every band) */
} wav_part_t;
/* what says whether t[] is the table of the part's sound lives outside the engine memory (another engine may use
 * that region while the part plays it, and a visit of ANALOG, which has no memory of its own, must not leave an old
 * table valid): wav_tick clears these as soon as the part's engine is not WAVELET */
static int16_t wav_snap[NPART][4];                  /* E0 .. E3 the table was built from */
static uint8_t wav_valid[NPART];                    /* 1: the part's t[] holds the table of wav_snap */
static uint8_t wav_xf[NPART];                       /* cross-fading to wav_scr.t: blocks left (WAV_XF .. 1), 0 none */
ENG_MEM_FITS("WAVELET", sizeof(wav_part_t));
#define WAV_PART(t) ((wav_part_t *)ENG_MEM((uint32_t)((t) - trk)))

/* --------------------------------------------------- the build (scratch) --- */
enum { WS_SRC, WS_DWT, WS_SPARSE, WS_BANDA, WS_BANDB, WS_DONE, WS_XF };
typedef struct {
    int16_t t[WAV_TSIZE];                           /* the table being built, then played by the cross-fade */
    int32_t x[WAV_N], a[WAV_N], c[WAV_N];           /* work: the source / a level's input, its output, the atoms */
    int16_t snap[4];                                /* E0 .. E3 being built */
    uint8_t owner;                                  /* part + 1, 0 = free */
    uint8_t stage;                                  /* WS_* */
    uint8_t band;                                   /* WS_BANDA / WS_BANDB: the layer, 0 .. WAV_NB - 1 */
    uint8_t ab;                                     /* 1: a[] is the level's input and x[] its output */
    uint16_t M;                                     /* the level: its length (WS_DWT: 512 down to 2; WS_BAND*: up) */
    uint16_t k;                                     /* the position in the level */
    uint32_t macs;                                  /* multiply-adds so far (the console, tests) */
} wav_scratch_t;
static wav_scratch_t wav_scr __attribute__((section(".pool")));

static inline int32_t wav_sat16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }

/* the source cycle into s->x (Q20): the wave at phase i / N + PHAS, mean removed, peak-normalised */
static void wav_source(wav_scratch_t *s)
{
    int32_t *x = s->x;
    uint32_t wave = (uint32_t)s->snap[0], e2 = (uint32_t)s->snap[2], i;
    uint32_t ph0 = wave == WW_PLS || wave == WW_TRI ? 0u : e2 * 33554432u;   /* PHAS: e2 / 128 of a cycle */
    uint32_t width = 85899346u + e2 * 32466857u;        /* WDTH: 2 % + e2 x 0.96 / 127 of a cycle */
    uint32_t inv1 = 0xFFFFFFFFu / (width >> 8), inv2 = 0xFFFFFFFFu / ((0u - width) >> 8);   /* ~2^40 / width */
    int32_t mean = 0, peak = 0;
    uint32_t scale;
    for (i = 0; i < WAV_N; i++) {
        uint32_t ph = (i << 23) + ph0;
        int32_t v;
        switch (wave) {
        case WW_SAW: v = (1 << 20) - (int32_t)(ph >> 11); break;
        case WW_SQR: v = ph < 0x80000000u ? 1 << 20 : -(1 << 20); break;
        case WW_PLS: v = ph < width ? 1 << 20 : -(1 << 20); break;
        case WW_TRI:
            if (ph < width)
                v = -(1 << 20) + (int32_t)(((uint64_t)ph * inv1) >> 19);
            else
                v = (1 << 20) - (int32_t)(((uint64_t)(ph - width) * inv2) >> 19);
            break;
        case WW_SIN: v = sine_i(ph) << 5; break;
        default: v = 0; break;
        }
        x[i] = v;
    }
    if (wave >= WW_NOIS) {                              /* the loop of the Wavelet Synthesizer's generator, shifted */
        uint32_t r = 12345u, sh = (e2 * 4u) & (WAV_N - 1u);
        for (i = 0; i < WAV_N; i++) {
            r = r * 1664525u + 1013904223u;
            x[(i + WAV_N - sh) & (WAV_N - 1u)] = (int32_t)(r >> 11) - (1 << 20);
        }
    }
    for (i = 0; i < WAV_N; i++)
        mean += x[i];
    mean = mean / (int32_t)WAV_N;
    for (i = 0; i < WAV_N; i++) {
        int32_t v = x[i] - mean, a = v < 0 ? -v : v;
        x[i] = v;
        if (a > peak)
            peak = a;
    }
    if (!peak)
        return;
    scale = 0x80000000u / (uint32_t)peak;               /* 2^31 / peak: x * scale >> 11 peaks at 2^20 */
    for (i = 0; i < WAV_N; i++)
        x[i] = (int32_t)(((int64_t)x[i] * (int32_t)scale) >> 11);
}

static int32_t wav_log2q8(uint32_t n)                   /* log2(n) in Q8, n >= 1 */
{
    uint32_t msb = 0, f;
    while (n >> (msb + 1u))
        msb++;
    f = msb >= 6u ? (n >> (msb - 6u)) & 63u : (n << (6u - msb)) & 63u;   /* the 6 bits under the top one */
    return (int32_t)(msb << 8) + WAV_LOG2[f];
}

/* keeps the K largest atoms, K = nz^(1 - SPRS / 127), nz the non-zero atoms (as the Wavelet Synthesizer:
 * the knob acts over its whole range whatever the wave and wavelet); ties go to the lower indices */
static void wav_sparsify(wav_scratch_t *s)
{
    int32_t *c = s->c, *m = s->a;
    uint32_t nz = 0, i, K, gap, ngt = 0, keep;
    int32_t thr, sprs = s->snap[3], mx = 0, tiny;
    c[0] = 0;                                           /* the approximation (the mean): dropped */
    for (i = 1; i < WAV_N; i++) {
        int32_t a = c[i] < 0 ? -c[i] : c[i];
        if (a > mx)
            mx = a;
    }
    tiny = mx >> 18;                                    /* under -108 dB of the largest: the rounding's leftovers (a
                                                         * few units) of atoms that are exact zeros in double
                                                         * precision. The VST counts atoms down to 1e-9: with the
                                                         * smoother wavelets it sees a few more tiny ones, so its
                                                         * SPARSITY law differs a little from this knob's */
    for (i = 1; i < WAV_N; i++) {
        int32_t a = c[i] < 0 ? -c[i] : c[i];
        if (a <= tiny)
            c[i] = 0;
        else
            nz++;
    }
    if (nz < 2u || sprs <= 0)
        return;
    {
        int32_t e = wav_log2q8(nz) * (127 - sprs) / 127;   /* log2(K), Q8 */
        uint32_t n = (uint32_t)e >> 8, f = (uint32_t)e & 255u;
        uint32_t p = WAV_POW2[f >> 2] + (((WAV_POW2[(f >> 2) + 1u] - WAV_POW2[f >> 2]) * (f & 3u)) >> 2);
        K = ((p << n) + 8192u) >> 14;
    }
    if (K < 1u)
        K = 1u;
    if (K >= nz)
        return;
    for (i = 0; i < WAV_N; i++)                         /* the magnitudes, sorted (shell sort, descending) */
        m[i] = i ? (c[i] < 0 ? -c[i] : c[i]) : -1;
    for (gap = 256; gap; gap >>= 1)
        for (i = gap; i < WAV_N; i++) {
            int32_t v = m[i];
            uint32_t j = i;
            while (j >= gap && m[j - gap] < v) {
                m[j] = m[j - gap];
                j -= gap;
            }
            m[j] = v;
        }
    thr = m[K - 1u];
    for (i = 1; i < WAV_N; i++) {
        int32_t a = c[i] < 0 ? -c[i] : c[i];
        if (a > thr)
            ngt++;
    }
    keep = K - ngt;                                     /* of those equal to the threshold, the first ones */
    for (i = 1; i < WAV_N; i++) {
        int32_t a = c[i] < 0 ? -c[i] : c[i];
        if (a < thr)
            c[i] = 0;
        else if (a == thr) {
            if (keep)
                keep--;
            else
                c[i] = 0;
        }
    }
}

/* engine B's atoms into s->c (Q20): band b (1 .. 9) holds 2^(b-1) atoms, as the Wavelet Synthesizer's patternCoeffs writes them
 * (Ramp all +1, Mirror +1 then -1 in each band, Alternate by band, Walsh by atom, Impulse the first atom, Random with the seed 1) */
static void wav_pattern(wav_scratch_t *s)
{
    int32_t *c = s->c;
    uint32_t pat = (uint32_t)s->snap[0] - WW_B0, b, k, i, r = 2654435761u + 12345u;   /* rng((1 x 2654435761 + 12345) >>> 0) */
    for (i = 0; i < WAV_N; i++)
        c[i] = 0;
    for (b = 1; b <= WAV_NB; b++) {
        uint32_t n = 1u << (b - 1u);
        for (k = 0; k < n; k++) {
            int32_t v;
            switch (pat) {
            case 0: v = 1 << 20; break;
            case 1: v = b == 1u ? 1 << 20 : (k < n / 2u ? 1 << 20 : -(1 << 20)); break;
            case 2: v = (b & 1u) ? 1 << 20 : -(1 << 20); break;
            case 3: v = (k & 1u) ? -(1 << 20) : 1 << 20; break;
            case 4: v = k == 0u ? 1 << 20 : 0; break;
            default:
                r = r * 1664525u + 1013904223u;
                v = (int32_t)(r >> 11) - (1 << 20);
                break;
            }
            c[n + k] = v;
        }
    }
}

/* the layer in s->x / s->a (the level's output, Q20) into column `band` of the table, and into DRY. Engine B: every layer is
 * scaled to a peak of 1.0 (16384), as the Wavelet Synthesizer does, so that the registration sets the balance of the bands */
static void wav_layer_out(wav_scratch_t *s, const int32_t *lay)
{
    uint32_t i;
    uint32_t f = 0, sh = 0;                             /* engine B: v = lay x 2^14 / peak, as (lay x f >> 16) >> sh */
    if (WAV_IS_B(s->snap[0])) {
        int32_t peak = 0;
        for (i = 0; i < WAV_N; i++) {
            int32_t a = lay[i] < 0 ? -lay[i] : lay[i];
            if (a > peak)
                peak = a;
        }
        if (peak > 0)
        {   /* (no 64-bit division on the FM-1: the divisor is cut to 16 bits, f = 2^30 / divisor, a 32-bit division) */
            uint32_t pk = peak > 0 ? (uint32_t)peak : 1u;
            while ((pk >> sh) >= 65536u)
                sh++;
            f = (1u << 30) / (pk >> sh);
        }
    }
    for (i = 0; i < WAV_N; i++) {
        int32_t v = f ? (int32_t)(((int64_t)lay[i] * (int32_t)f + 32768) >> 16) : wav_sat16((lay[i] + 32) >> 6);
        if (f)
            v = (v + (int32_t)((1u << sh) >> 1)) >> sh;
        v = wav_sat16(v);
        int16_t *row = s->t + i * WAV_NL;
        row[s->band] = (int16_t)v;
        if (!WAV_IS_B(s->snap[0]))                      /* (engine B has no dry cycle: its DRY layer stays 0) */
            row[WAV_DRY] = (int16_t)wav_sat16(row[WAV_DRY] + v);
    }
}

static uint8_t wav_done(wav_scratch_t *s)               /* the last layer is in: the guard row; WS_DONE */
{
    uint32_t i;
    for (i = 0; i < WAV_NL; i++)
        s->t[WAV_N * WAV_NL + i] = s->t[i];
    return WS_DONE;
}

/* one slice of the build: at most `budget` multiply-adds (a sort counts as the whole budget) */
static __attribute__((noinline)) void wav_build_step(wav_scratch_t *s, int32_t budget)
{
    const int32_t *h = WAV_H[(uint32_t)s->snap[1] % WAV_NW], *g = WAV_G[(uint32_t)s->snap[1] % WAV_NW];
    uint32_t L = WAV_L[(uint32_t)s->snap[1] % WAV_NW], n, i;
    while (budget > 0) {
        int32_t *in = s->ab ? s->a : s->x, *out = s->ab ? s->x : s->a;
        switch (s->stage) {
        case WS_SRC:
            for (i = 0; i < WAV_TSIZE; i++)
                s->t[i] = 0;
            if (WAV_IS_B(s->snap[0])) {                 /* engine B: the atoms are a pattern, no transform, no sparsity */
                wav_pattern(s);
                s->stage = WS_SPARSE;
                budget -= 1024;
                break;
            }
            wav_source(s);
            s->stage = WS_DWT;
            s->M = (uint16_t)WAV_N;
            s->k = 0;
            s->ab = 0;
            budget -= 2048;
            break;
        case WS_DWT: {                                  /* the level M: in[M] -> out[M / 2] + c[M / 2 ..] */
            uint32_t M = s->M, half = M >> 1, k = s->k, mask = M - 1u;
            while (k < half && budget > 0) {
                int64_t sa = 0, sd = 0;
                for (n = 0; n < L; n++) {
                    int32_t v = in[(2u * k + n) & mask];
                    sa += (int64_t)h[n] * v;
                    sd += (int64_t)g[n] * v;
                }
                out[k] = (int32_t)((sa + (1 << 27)) >> 28);
                s->c[half + k] = (int32_t)((sd + (1 << 27)) >> 28);
                budget -= (int32_t)L;
                k++;
            }
            s->k = (uint16_t)k;
            if (k >= half) {                            /* the level is done */
                s->ab ^= 1u;
                s->M = (uint16_t)half;
                s->k = 0;
                if (half <= 1u) {
                    s->stage = WS_SPARSE;
                }
            }
            break;
        }
        case WS_SPARSE:
            if (!WAV_IS_B(s->snap[0]))
                wav_sparsify(s);
            s->stage = WS_BANDA;
            s->band = 0;
            s->M = 2;
            s->k = 0;
            s->ab = 0;
            for (i = 0; i < 2u; i++)
                s->a[i] = 0;
            budget -= 8192;
            break;
        case WS_BANDA: {                                /* band's detail atoms c[half ..] -> out[M], g only */
            uint32_t M = s->M, half = M >> 1, k = s->k, mask = M - 1u;
            while (k < half && budget > 0) {
                int32_t dk = s->c[half + k];
                if (dk) {
                    for (n = 0; n < L; n++)
                        out[(2u * k + n) & mask] += (int32_t)(((int64_t)g[n] * dk + (1 << 27)) >> 28);
                    budget -= (int32_t)L;
                } else
                    budget--;
                k++;
            }
            s->k = (uint16_t)k;
            if (k >= half) {
                s->ab ^= 1u;                            /* out becomes the next level's input */
                s->k = 0;
                if (M >= WAV_N) {                       /* (band 9: its level is the cycle) */
                    wav_layer_out(s, out);
                    s->band++;
                    s->stage = s->band < WAV_NB ? WS_BANDA : wav_done(s);
                    s->M = (uint16_t)(2u << s->band);
                    s->ab = 0;
                    for (i = 0; i < (uint32_t)s->M && s->band < WAV_NB; i++)
                        s->a[i] = 0;
                } else {
                    s->stage = WS_BANDB;
                    s->M = (uint16_t)(M << 1);
                    for (i = 0; i < (uint32_t)s->M; i++)
                        (s->ab ? s->x : s->a)[i] = 0;
                }
            }
            break;
        }
        case WS_BANDB: {                                /* the levels above: in[M / 2] -> out[M], h only */
            uint32_t M = s->M, half = M >> 1, k = s->k, mask = M - 1u;
            while (k < half && budget > 0) {
                int32_t ak = in[k];
                if (ak) {
                    for (n = 0; n < L; n++)
                        out[(2u * k + n) & mask] += (int32_t)(((int64_t)h[n] * ak + (1 << 27)) >> 28);
                    budget -= (int32_t)L;
                } else
                    budget--;
                k++;
            }
            s->k = (uint16_t)k;
            if (k >= half) {
                s->ab ^= 1u;
                s->k = 0;
                if (M >= WAV_N) {                       /* the layer is complete */
                    wav_layer_out(s, out);
                    s->band++;
                    s->stage = s->band < WAV_NB ? WS_BANDA : wav_done(s);
                    s->M = (uint16_t)(2u << s->band);
                    s->ab = 0;
                    for (i = 0; i < (uint32_t)s->M && s->band < WAV_NB; i++)
                        s->a[i] = 0;
                } else {
                    s->M = (uint16_t)(M << 1);
                    for (i = 0; i < (uint32_t)s->M; i++)
                        (s->ab ? s->x : s->a)[i] = 0;
                }
            }
            break;
        }
        default:
            return;
        }
    }
    s->macs += (uint32_t)(WAV_QUANTUM - budget);
}

/* -------------------------------------------------------------- gains --- */
static int32_t wav_softplus(int32_t xq8)                /* ln(1 + e^4x) / 4, x in bands Q8 */
{
    int32_t i, f;
    if (xq8 >= 2560)
        return xq8;
    if (xq8 <= -1024)
        return 0;
    xq8 += 1024;
    i = xq8 >> 5;
    f = xq8 & 31;
    return WAV_SOFTPLUS[i] + (((WAV_SOFTPLUS[i + 1] - WAV_SOFTPLUS[i]) * f) >> 5);
}
static int32_t wav_resbump(int32_t dq8)                 /* exp(-d^2 / 0.72), d >= 0 in bands Q8, Q15 */
{
    int32_t i = dq8 >> 4, f = dq8 & 15;
    if (dq8 >= 768)
        return 0;
    return WAV_RESBUMP[i] + (((WAV_RESBUMP[i + 1] - WAV_RESBUMP[i]) * f) >> 4);
}
static int32_t wav_db_to_q10(int32_t dbq8)              /* 10^(dB / 20) in Q10, 0 .. WAV_GMAX */
{
    int32_t o = (int32_t)(((int64_t)dbq8 * 10886) >> 16);   /* octaves Q8: dB x log2(10) / 20 */
    int32_t n = o >> 8, f = o & 255;
    uint32_t p = WAV_POW2[f >> 2] + (((WAV_POW2[(f >> 2) + 1] - WAV_POW2[f >> 2]) * (uint32_t)(f & 3)) >> 2);
    if (n >= 4)
        return WAV_GMAX;
    if (n < -20)
        return 0;
    return (int32_t)(p >> (uint32_t)(4 - n));           /* p (Q14) x 2^n / 16 */
}

/* the nine band gains (Q10) of a voice: the scale filter at cutq8 (bands, Q8), then the bands whose
 * harmonics would pass Nyquist at this pitch faded out (band b: fully there while 2^(b-1) harmonics
 * fit, gone when 2^(b-2) do not: the anti-aliasing, one octave coarse) */
static void wav_gains(const int16_t *p, int32_t cutq8, uint32_t inc, int32_t *g, int32_t *gate)
{
    uint32_t flt = (uint32_t)clamp(p[P_E6], 0, 17), type = flt / 6u, j;
    int32_t slope = WAV_SLOPE[flt % 6u], resq8 = clamp(p[P_E5], 0, 127) * 6144 / 127;
    uint32_t hmaxq8 = 0x40000000u / ((inc >> 9) + 1u);  /* harmonics below Nyquist, Q8 (2^31 / inc) */
    for (j = 0; j < WAV_NB; j++) {
        int32_t d = (int32_t)((j + 1u) << 8) - cutq8, ad = d < 0 ? -d : d, sp, att, boost, gain;
        uint32_t r;
        sp = type == 0u ? wav_softplus(d) : type == 1u ? wav_softplus(-d) : wav_softplus(ad - 128);
        att = slope * sp;
        if (att > 140 << 8)
            att = 140 << 8;
        boost = (resq8 * wav_resbump(ad)) >> 15;
        gain = wav_db_to_q10(boost - att);
        r = j ? hmaxq8 >> (j - 1u) : hmaxq8 << 1;
        if (r < 512u)
            gain = r <= 256u ? 0 : (gain * (int32_t)((r - 256u) << 7)) >> 15;
        g[j] = gain;
        gate[j] = r >= 512u ? 1024 : r <= 256u ? 0 : (int32_t)((r - 256u) << 2);   /* the same fade, alone (Q10) */
    }
}

/* 10^(dB / 20) in Q10 for dB (Q8) from -120 .. +72: engine B's tilt reaches +8 bands x ... (the plain one stops at +24 dB) */
static int32_t wav_db_to_q10w(int32_t dbq8)
{
    int32_t o = (int32_t)(((int64_t)dbq8 * 10886) >> 16);   /* octaves Q8 */
    int32_t n = o >> 8, f = o & 255;
    uint32_t p = WAV_POW2[f >> 2] + (((WAV_POW2[(f >> 2) + 1] - WAV_POW2[f >> 2]) * (uint32_t)(f & 3)) >> 2);   /* Q14 */
    if (n < -20)
        return 0;
    if (n > 12)
        n = 12;
    return n >= 4 ? (int32_t)(p << (uint32_t)(n - 4)) : (int32_t)(p >> (uint32_t)(4 - n));
}

/* engine B: the gain of each band of a voice, from the filter's gains g (in) to the final ones (out), Q10.
 *   bar_j  = registration_j x tilt^j       TILT: 0 .. -24 dB per band over E3 0 .. 127
 *   norm   = max(1, sum of bar)            (as the VST: the sum of the bars, never boosting)
 *   gain_j = filter_j x bar_j / norm x lv_j  the scale envelope lv_j: it swells with the amp ATK, then, with FADE (E7 > 0),
 *            falls to half the faster the higher the band (E7 127: the top band in a millisecond, the lowest in 10 s) */
static void wav_bscale(const int16_t *p, wav_part_t *ps, uint32_t vi, int32_t *g)
{
    const uint16_t *reg = WAV_REG[(uint32_t)clamp(p[P_E2], 0, 127) >> 3];
    int32_t tilt_q8 = -(clamp(p[P_E3], 0, 127) * 6144 / 127);   /* dB per band, Q8: 0 .. -24 dB (6144 = 24 x 256) */
    int32_t fade = clamp(p[P_E7], 0, 127), idx7 = 127 - fade;
    int32_t bar[WAV_NB], sum = 0, norm;
    uint16_t *lv = ps->lv[vi];
    uint32_t j;
    for (j = 0; j < WAV_NB; j++) {
        bar[j] = ((int32_t)reg[j] * wav_db_to_q10w(tilt_q8 * (int32_t)j)) >> 10;   /* Q10, at most 1024 */
        sum += bar[j];
    }
    norm = sum > 1024 ? sum : 1024;
    if (ps->atk[vi]) {                                  /* the swell: every band the same, over the amp attack */
        int32_t step = (int32_t)(ENV_LIN[clamp(p[P_ATK], 0, 127)] >> 9) + 1;
        int32_t l = (int32_t)lv[0] + step;
        if (l >= 32767) {
            l = 32767;
            ps->atk[vi] = 0;
        }
        for (j = 0; j < WAV_NB; j++)
            lv[j] = (uint16_t)l;
    } else if (fade > 0) {                              /* the fall to half, a time per band: idx7 at the top, 127 at the bottom */
        for (j = 0; j < WAV_NB; j++) {
            int32_t idx = idx7 + ((127 - idx7) * (int32_t)(WAV_NB - 1u - j)) / (int32_t)(WAV_NB - 1u);
            int32_t l = (int32_t)lv[j];
            l += ((16384 - l) * (int32_t)ENV_EXP[clamp(idx, 0, 127)]) >> 16;   /* (|16384 - l| x 65535 < 2^31) */
            lv[j] = (uint16_t)clamp(l, 0, 32767);
        }
    }
    for (j = 0; j < WAV_NB; j++)
        g[j] = ((g[j] * bar[j] / norm) * (int32_t)lv[j]) >> 15;   /* (g <= 16384, bar <= 1024: 2^24; then x lv <= 2^15) */
}

/* ------------------------------------------------------------- voices --- */
static void wavelet_note_on(track_t *t, voice_t *v)
{
    wav_part_t *ps = WAV_PART(t);
    uint32_t vi = (uint32_t)(v - t->v) % NVOICE, j;
    v->s[0] = 0;                                        /* the gains start where this block puts them */
    ps->atk[vi] = 1;                                    /* engine B: the scale envelope swells again */
    for (j = 0; j < WAV_NB; j++)
        ps->lv[vi][j] = 0;
}

static const param_desc_t WAVELET_WIDTH = {"WDTH", F_PCT, 0, 127, 0, 0, 0};
static const param_desc_t WAVELET_BARS = {"BARS", F_INT, 0, 127, 0, 0, 0};   /* engine B: the registration, E2 / 8 (0 .. 15) */
static const param_desc_t WAVELET_TILT = {"TILT", F_PCT, 0, 127, 0, 0, 0};   /* .. 0 = flat, 127 = -24 dB per band */
static const param_desc_t WAVELET_FADE = {"FADE", F_PCT, 0, 127, 0, 0, 0};   /* .. 0 = none */
static const param_desc_t *wavelet_desc(const track_t *t, uint32_t k)
{
    if (WAV_IS_B(t->p[P_E0]))
        return k == 2u ? &WAVELET_BARS : k == 3u ? &WAVELET_TILT : k == 7u ? &WAVELET_FADE : 0;
    if (k == 2u && (t->p[P_E0] == WW_PLS || t->p[P_E0] == WW_TRI))
        return &WAVELET_WIDTH;
    return 0;
}

/* the bands that sound (gain not 0): the row sums skip the others, which is exact (a band of gain 0 adds 0); a band is also
 * dropped from the list while its gain is 0 at both ends of the ramp. Many sounds have bands filtered out or gated by the pitch */
typedef struct { int32_t g[WAV_NB]; uint8_t act[WAV_NB]; uint8_t n; } wav_gs_t;
static void wav_gs_set(wav_gs_t *a, const int32_t *g)
{
    uint32_t j;
    a->n = 0;
    for (j = 0; j < WAV_NB; j++) {
        a->g[j] = g[j];
        if (g[j])
            a->act[a->n++] = (uint8_t)j;
    }
}
/* a row of the table x the band gains (Q10): Q24 */
static inline int32_t wav_row(const int16_t *r, const wav_gs_t *a)
{
    const int32_t *g = a->g;
    uint32_t k;
    int32_t s;
    if (a->n == WAV_NB)
        return g[0] * r[0] + g[1] * r[1] + g[2] * r[2] + g[3] * r[3] + g[4] * r[4] + g[5] * r[5] + g[6] * r[6] +
               g[7] * r[7] + g[8] * r[8];
    for (s = 0, k = 0; k < a->n; k++)
        s += g[a->act[k]] * r[a->act[k]];
    return s;
}
typedef struct { uint32_t i; int32_t s0, s1; } wav_rd_t;   /* the rows last summed: row i and i + 1 */
#define WAV_RD_NONE 0x7FFFFFFFu                          /* no rows summed (its + 1 is no row either) */

/* one sample of a table at phase ph with band gains g (Q10): the sum, Q15. The two rows the sample sits between
 * are summed once: a sample on the same rows as the last one (below 86 Hz) or one row on (up to 172 Hz) reuses what
 * was summed (c), so the nine multiplies run once or twice a sample, not twice always. Called with the filter's
 * gains for the sound and, under the window, with the anti-aliasing gate alone for the dry cycle */
static inline int32_t wav_read(const int16_t *T, uint32_t ph, const wav_gs_t *g, wav_rd_t *c)
{
    uint32_t i = ph >> (32u - WAV_LOG2N);
    const int16_t *r = T + i * WAV_NL, *r1 = r + WAV_NL;
    int32_t fr = (int32_t)((ph >> 8) & 0x7FFFu), s0, s1;
    if (i == c->i) {
        s0 = c->s0;
        s1 = c->s1;
    } else {
        s0 = i == c->i + 1u ? c->s1 : wav_row(r, g);
        s1 = wav_row(r1, g);
        c->i = i;
        c->s0 = s0;
        c->s1 = s1;
    }
    return (s0 >> 9) + (int32_t)(((int64_t)(s1 - s0) * fr) >> 24);
}

static void wavelet_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    wav_part_t *ps = WAV_PART(t);
    uint32_t pi = (uint32_t)(t - trk) % NPART;
    const int16_t *p = t->p;
    const int16_t *T = ps->t, *T2 = 0;
    int16_t *g0 = ps->g0[(uint32_t)(v - t->v) % NVOICE];
    int32_t g1[WAV_NB], g[WAV_NB];
    wav_gs_t gs, gts;
    int32_t cutq8 = (clamp(p[P_E4], 0, 127) * 3072 + m->cutoff * 24) / 127;
    int32_t pd = WAV_IS_B(p[P_E0]) ? 0 : clamp(p[P_E7], 0, 127) * 258;   /* WNDW depth, Q15 (engine B: E7 is FADE, no window) */
    uint32_t ph = v->ph[0], inc = m->inc, i, j;
    uint32_t pc = (uint32_t)(m->shape - (64 << 8)) << 17;   /* the window's centre: 0 at SHAPE's default (the VST's POSITION 0);
                                                         * the SHP amounts move it, a whole cycle over the knob's range */
    uint32_t hwsh = 2;                                  /* the window's half width: 2^(32 - hwsh), a quarter cycle */
    int32_t xbase = 0, xstep = 0;                       /* cross-fade: the new table's share, Q15, per sample */
    wav_rd_t c1 = {WAV_RD_NONE, 0, 0}, c2 = c1, d1 = c1, d2 = c1;   /* the filtered sound's rows, the dry cycle's */
    int32_t gate[WAV_NB];
    if (!wav_valid[pi])
        return;
    wav_gains(p, cutq8, inc, g1, gate);
    if (WAV_IS_B(p[P_E0]))
        wav_bscale(p, ps, (uint32_t)(v - t->v) % NVOICE, g1);
    if (!v->s[0]) {
        for (j = 0; j < WAV_NB; j++)
            g0[j] = (int16_t)g1[j];
        v->s[0] = 1;
    }
    if (wav_xf[pi] && wav_scr.owner == pi + 1u && wav_scr.stage == WS_XF) {
        T2 = wav_scr.t;
        xbase = (int32_t)(WAV_XF - wav_xf[pi]) * (32768 / (int32_t)WAV_XF);
        xstep = (32768 / (int32_t)WAV_XF) / (int32_t)(n ? n : 1u);
    }
    while (hwsh > 1u && (inc >> (29u - hwsh)) != 0u)    /* at least 8 samples each side at high pitches */
        hwsh--;
    for (i = 0; i < n; i++) {
        int32_t y, dry = 0;
        if (!(i & 7u)) {                                /* the gains: four steps over the block */
            int32_t q = (int32_t)(i >> 3) + 1;
            for (j = 0; j < WAV_NB; j++)
                g[j] = g0[j] + (((g1[j] - g0[j]) * q) >> 2);
            wav_gs_set(&gs, g);
            if (pd)
                wav_gs_set(&gts, gate);
            c1.i = c2.i = d1.i = d2.i = WAV_RD_NONE;    /* (the sums are of the old gains) */
        }
        y = wav_read(T, ph, &gs, &c1);
        if (pd)                                         /* the dry cycle, band-limited by the gate alone */
            dry = wav_read(T, ph, &gts, &d1);
        if (T2) {
            int32_t xw = xbase + (int32_t)i * xstep;
            /* (64 bits: the difference of two Q15 values times a Q15 share can pass 2^31) */
            y += (int32_t)(((int64_t)(wav_read(T2, ph, &gs, &c2) - y) * xw) >> 15);
            if (pd)
                dry += (int32_t)(((int64_t)(wav_read(T2, ph, &gts, &d2) - dry) * xw) >> 15);
        }
        if (pd) {                                       /* the window: dry outside, the filtered sum inside */
            uint32_t d = ph - pc;
            int32_t w = 32767 - pd;
            if (d > 0x80000000u)
                d = 0u - d;
            if (d < (1u << (32u - hwsh))) {
                int32_t bump = (32767 + sine_i((d << hwsh >> 1) + 0x40000000u)) >> 1;
                w += (pd * bump) >> 15;
            }
            y = dry + (int32_t)(((int64_t)(y - dry) * w) >> 15);
        }
        out[i] += voice_amp(soft_knee(y, 24000), m, i);
        ph += inc;
    }
    v->ph[0] = ph;
    for (j = 0; j < WAV_NB; j++)
        g0[j] = (int16_t)g1[j];
}

static const engine_t ENG_WAVELET = {
    .name = "WAVELET",
    .page_title = {"ATOMS", "SCALE"},
    .edit = {
        {"WAVE", F_ENUM, 0, 11, 0, N_WWAVE, 0},         /* 0 .. 5 engine A, 6 .. 11 engine B */
        {"WVLT", F_ENUM, 0, WAV_NW - 1, 2, N_WVLT, 0},
        {"PHAS", F_PCT, 0, 127, 0, 0, 0},               /* WDTH on PLS / TRI, BARS in engine B (wavelet_desc) */
        {"SPRS", F_PCT, 0, 127, 0, 0, 0},               /* TILT in engine B */
        {"CUT", F_INT, 0, 127, 127, 0, 0},
        {"RES", F_PCT, 0, 127, 0, 0, 0},
        {"FLT", F_ENUM, 0, 17, 2, N_WFLT, 0},
        {"WNDW", F_PCT, 0, 127, 0, 0, 0},               /* FADE in engine B */
    },
    .presets = WAVELET_PRESETS,
    .npresets = NELEM(WAVELET_PRESETS),
    .note_on = wavelet_note_on,
    .render = wavelet_render,
    .knob = {P_E4, P_E5, P_E3, P_REL},
    .poly = 4,                                          /* a voice costs about twice ANALOG's: 4 voices = ANALOG's 8 (the FM-1 is small: no overload) */
    .keep = 0x01,                                       /* the gains' ramp goes on over a retrigger */
    .desc = wavelet_desc,
    .mem = sizeof(wav_part_t),
};
