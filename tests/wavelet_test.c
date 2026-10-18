/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Pierre Arnaud GUYOT (OktoPierre)
 * The minimal voice host below (track, voice_on, env_tick, the phrase) follows tests/fe_host.c of Felucca-Engines,
 * MIT, Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments (firmware/engines/LICENSE). */
/* The WAVELET engine on a minimal host (after Felucca-Engines' tests/fe_host.c): no Felucca sources but
 * dsp.c, eng_analog.c (a cost reference) and the engine itself, so it builds anywhere with a C compiler.
 *   cc -O2 -std=gnu99 -I build/host -o build/host/wavelet_test tests/wavelet_test.c
 *   build/host/wavelet_test [OUT_DIR]
 * 1. every WAVE x WVLT (x SPRS 0 / 64): the table builds through wav_tick in bounded slices; its DRY row
 *    peaks near 1.0 and is the sum of the layers; the cost (multiply-adds, blocks) is printed.
 * 2. the tables of a few sounds are written as text (OUT_DIR/table_*.txt) for tools/wavelet_ref.js, the
 *    floating-point reference of the same reduced algorithm.
 * 3. every preset plays the phrase of fe_host.c: a hash, the peak (never near full scale, never silent),
 *    the voices free after the release; OUT_DIR/preset_NN.wav of the first ones; the time per sample
 *    against ANALOG's on the same phrase.
 * 4. a table change while a note plays: the cross-fade shows no step (the largest sample-to-sample jump
 *    across the switch stays below the sound's own).
 * 5. above Nyquist: at the highest pitch the bands that would alias are off. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NVOICE 8
#define NPART 4
#include "fe_tables.h"
#include "../firmware/engines/src/felucca_engine.h"

enum { P_ATK, P_REL, P_FTYPE, P_E0, P_E1, P_E2, P_E3, P_E4, P_E5, P_E6, P_E7, P_COUNT };
typedef struct track {
    int16_t p[P_COUNT];
    voice_t v[NVOICE];
    uint8_t engine;
} track_t;
static track_t trk[NPART];

#include "../firmware/engines/src/dsp.c"
#define ENG_MEM_PART 12888u
static union { uint64_t align; uint8_t b[ENG_MEM_PART]; } eng_mem[NPART];
#define ENG_MEM(p) ((void *)eng_mem[p].b)
#define ENG_MEM_FITS(name, size) _Static_assert((size) <= ENG_MEM_PART, name " state over ENG_MEM_PART")
#define NENGINES 2
#include "../firmware/engines/src/eng_analog.c"
#include "../firmware/src/eng_wavelet.c"
static const engine_t *const ENGINES[NENGINES] = {&ENG_ANALOG, &ENG_WAVELET};
static inline uint32_t eng_idx(uint32_t e) { return e < NENGINES ? e : 0u; }
#include "../firmware/src/wavelet_tick.c"
#define E_WAV 1u

static int fails;
static int check(const char *what, int ok)
{
    if (!ok) {
        printf("FAIL: %s\n", what);
        fails++;
    }
    return ok;
}

/* ------------------------------------------------- a minimal voice host --- */
static uint32_t vage;
static void voice_on(track_t *t, const engine_t *e, voice_t *v, uint32_t note, int32_t fine)
{
    int sounding = v->active && v->stage != 0;
    uint32_t ph0 = v->ph[0], ph1 = v->ph[1], ph2 = v->ph[2];
    int32_t s[8];
    memcpy(s, v->s, sizeof s);
    v->note = (uint8_t)note;
    v->vel = 100;
    v->gate = 1;
    v->active = 1;
    v->stage = 1;
    v->age = ++vage;
    v->pitch16 = v->pitch_cur = (int32_t)note * 16;
    v->fine = fine;
    if (!sounding)
        v->env = v->env_out = 0;
    e->note_on(t, v);
    if (sounding) {
        uint32_t k;
        v->ph[0] = ph0;
        v->ph[1] = ph1;
        v->ph[2] = ph2;
        for (k = 0; k < 8u; k++)
            if (k != 2u && k != 3u && (e->keep >> k & 1u))
                v->s[k] = s[k];
    }
}
static int32_t env_tick(voice_t *v)
{
    if (!v->active)
        return 0;
    switch (v->stage) {
    case 1:
        v->env += 1 << 22;
        if (v->env >= 1 << 24) { v->env = 1 << 24; v->stage = 2; }
        break;
    case 2:
        v->env -= (v->env - (3 << 22)) >> 3;
        break;
    case 3:
        v->env -= v->env >> 4;
        if (v->env < (1 << 24) / 1000) { v->env = 0; v->stage = 0; v->active = 0; }
        break;
    default:
        break;
    }
    return v->env >> 9;
}

/* one block of part t: wav_tick (as mix_block does), then its voices; returns the voices rendered */
static uint32_t render_block(track_t *t, int32_t *out, uint32_t b, int32_t *last, int sweep)
{
    const engine_t *e = ENGINES[t->engine];
    uint32_t k, nr = 0;
    wav_tick();
    if (e->block)
        e->block(t);
    memset(out, 0, CTL * sizeof *out);
    for (k = 0; k < NVOICE; k++) {
        voice_t *v = &t->v[k];
        vmod_t m;
        int32_t a;
        uint32_t base;
        if (!v->active)
            continue;
        a = env_tick(v);
        if (e->amp)
            a = e->amp(t, v, a);
        if (!v->active)
            continue;
        base = pitch_inc((uint32_t)clamp(v->pitch16, 0, 2047));
        memset(&m, 0, sizeof m);
        m.pitch16 = v->pitch16;
        m.inc = base + (uint32_t)((int32_t)(base >> 12) * v->fine);
        m.fine = v->fine;
        m.amp0 = last[k];
        m.amp1 = a;
        m.envq15 = a;
        m.cutoff = sweep ? ((int32_t)(b % 96u) - 48) << 7 : 0;
        m.shape = (64 << 8) + (sweep ? ((int32_t)(b % 64u) - 32) << 8 : 0);
        last[k] = a;
        e->render(t, v, out, CTL, &m);
        nr++;
    }
    return nr;
}

static void part_reset(track_t *t, uint32_t engine)
{
    memset(t, 0, sizeof *t);
    memset(eng_mem[t - trk].b, 0, ENG_MEM_PART);
    {   /* a part that was never WAVELET (or left it): no table, no build, as wav_tick leaves it */
        uint32_t pi = (uint32_t)(t - trk);
        wav_valid[pi] = 0;
        wav_xf[pi] = 0;
        if (wav_scr.owner == pi + 1u)
            wav_scr.owner = 0;
    }
    t->engine = (uint8_t)engine;
    vage = 0;
}

/* builds part t's table through wav_tick alone; returns the blocks it took (0 = never) */
static uint32_t build_table(track_t *t)
{
    uint32_t b, pi = (uint32_t)(t - trk);
    for (b = 1; b < 4000u; b++) {
        wav_tick();
        if (wav_valid[pi] && !wav_scr.owner)
            return b;
    }
    return 0;
}

static uint64_t fnv(uint64_t h, const int32_t *x, uint32_t n)
{
    uint32_t i, k;
    for (i = 0; i < n; i++)
        for (k = 0; k < 4u; k++) {
            h ^= ((uint32_t)x[i] >> (8u * k)) & 0xFFu;
            h *= 1099511628211ull;
        }
    return h;
}

/* ------------------------------------------------------------- WAV out --- */
static void wav_write(const char *path, const int16_t *s, uint32_t n)
{
    FILE *f = fopen(path, "wb");
    uint32_t v;
    if (!f)
        return;
#define U32(x) v = (x); fwrite(&v, 4, 1, f)
#define U16(x) v = (x); fwrite(&v, 2, 1, f)
    fwrite("RIFF", 1, 4, f); U32(36 + n * 2); fwrite("WAVEfmt ", 1, 8, f); U32(16); U16(1); U16(1);
    U32(FS); U32(FS * 2); U16(2); U16(16); fwrite("data", 1, 4, f); U32(n * 2);
    fwrite(s, 2, n, f);
    fclose(f);
}

#define NBLK 300
static int16_t wavbuf[NBLK * CTL];

typedef struct { uint64_t h; int32_t peak, maxjump; uint32_t free_at; } result_t;
static result_t render_phrase(const engine_t *e, const preset_t *pr, uint32_t part, int sweep, int tabswitch)
{
    track_t *t = &trk[part];
    int32_t out[CTL], last[NVOICE] = {0}, prev = 0;
    uint32_t b, i, k;
    result_t r = {14695981039346656037ull, 0, 0, 0};
    part_reset(t, e == &ENG_WAVELET ? E_WAV : 0u);
    for (k = 0; k < 8u; k++)
        t->p[P_E0 + k] = pr->e[k];
    t->p[P_ATK] = pr->env[0];
    t->p[P_REL] = pr->env[3];
    if (e == &ENG_WAVELET)
        build_table(t);                                 /* (the firmware: silent until it is there) */
    for (b = 0; b < NBLK; b++) {
        if (b == 0)
            voice_on(t, e, &t->v[0], 43, 0);
        if (b == 30)
            voice_on(t, e, &t->v[1], 62, 23);
        if (b == 70)
            voice_on(t, e, &t->v[0], 50, 0);
        if (b == 100)
            voice_on(t, e, &t->v[2], 79, -17);
        if (tabswitch && b == 60)
            if (tabswitch == 2) {
                t->p[P_E2] = (int16_t)((t->p[P_E2] + 64) & 127);   /* the source's phase flips: a rebuild and a cross-fade */
            } else {
                t->p[P_E1] = 2;                         /* another wavelet (DB4): a rebuild and a cross-fade */
            }
        if (b == 150)
            for (k = 0; k < NVOICE; k++)
                if (t->v[k].active) { t->v[k].gate = 0; t->v[k].stage = 3; }
        render_block(t, out, b, last, sweep);
        r.h = fnv(r.h, out, CTL);
        for (i = 0; i < CTL; i++) {
            int32_t a = out[i] < 0 ? -out[i] : out[i], j = out[i] - prev;
            if (a > r.peak)
                r.peak = a;
            if (j < 0)
                j = -j;
            if (b >= 20u && b <= 140u && j > r.maxjump)    /* steady part (not the attacks) */
                r.maxjump = j;
            prev = out[i];
            wavbuf[b * CTL + i] = (int16_t)clamp(out[i], -32768, 32767);
        }
        if (!r.free_at && b > 150u) {
            int any = 0;
            for (k = 0; k < NVOICE; k++)
                any |= t->v[k].active;
            if (!any)
                r.free_at = b;
        }
    }
    return r;
}

int main(int argc, char **argv)
{
    const char *outdir = argc > 1 ? argv[1] : "build/host";
    char path[512];
    uint32_t w, wl, sp, j, i, n;
    double t_wav = 0, t_ana = 0;
    uint32_t n_wav = 0, n_ana = 0;
    printf("WAVELET host test: table %u B, part state %u B, scratch %u B, quantum %d\n",
           (unsigned)(WAV_TSIZE * 2u), (unsigned)sizeof(wav_part_t), (unsigned)sizeof(wav_scratch_t), WAV_QUANTUM);

    /* 1. every wave x wavelet builds; 2. a few tables as text */
    for (w = 0; w < 6u; w++)
        for (wl = 0; wl < WAV_NW; wl++)
            for (sp = 0; sp < 2u; sp++) {
                track_t *t = &trk[0];
                wav_part_t *ps;
                uint32_t blocks, macs;
                int32_t peak = 0, maxlay = 0, sumerr = 0;
                part_reset(t, E_WAV);
                t->p[P_E0] = (int16_t)w;
                t->p[P_E1] = (int16_t)wl;
                t->p[P_E2] = (int16_t)(w == 2u || w == 3u ? 40 : 17);
                t->p[P_E3] = (int16_t)(sp ? 64 : 0);
                blocks = build_table(t);
                macs = wav_scr.macs;
                ps = WAV_PART(t);
                for (i = 0; i < WAV_N; i++) {
                    const int16_t *row = ps->t + i * WAV_NL;
                    int32_t s = 0, d = row[WAV_DRY], ad = d < 0 ? -d : d;
                    for (j = 0; j < WAV_NB; j++) {
                        int32_t a = row[j] < 0 ? -row[j] : row[j];
                        s += row[j];
                        if (a > maxlay)
                            maxlay = a;
                    }
                    if (ad > peak)
                        peak = ad;
                    s -= d;
                    if (s < 0)
                        s = -s;
                    if (s > sumerr)
                        sumerr = s;
                }
                snprintf(path, sizeof path, "build wave %u wavelet %u sprs %u", w, wl, sp);
                check(path, blocks > 0);
                snprintf(path, sizeof path, "DRY peak near 1.0 (wave %u wavelet %u sprs %u: %d)", w, wl, sp, peak);
                check(path, sp ? peak >= 2000 && peak <= 32767 : peak >= 16000 && peak <= 16500);
                snprintf(path, sizeof path, "DRY = sum of the layers (wave %u wavelet %u: %d)", w, wl, sumerr);
                check(path, sumerr == 0);
                snprintf(path, sizeof path, "guard row (wave %u wavelet %u)", w, wl);
                check(path, !memcmp(ps->t, ps->t + WAV_N * WAV_NL, WAV_NL * sizeof(int16_t)));
                printf("table wave %u wvlt %-5s sprs %-3d: %4u blocks %7u macs, DRY peak %5d, layer peak %5d\n",
                       w, N_WVLT[wl], t->p[P_E3], blocks, macs, peak, maxlay);
                if ((w == 0u || w == 2u || w == 5u) && (wl == 0u || wl == 2u || wl == 6u)) {
                    FILE *f;
                    snprintf(path, sizeof path, "%s/table_w%u_l%u_s%d.txt", outdir, w, wl, t->p[P_E3]);
                    f = fopen(path, "w");
                    if (f) {
                        fprintf(f, "# wave %u wavelet %u e2 %d sprs %d\n", w, wl, t->p[P_E2], t->p[P_E3]);
                        fprintf(f, "# atoms");          /* the atoms kept (wav_scr.c is still the build's) */
                        for (i = 1; i < WAV_N; i++)
                            if (wav_scr.c[i])
                                fprintf(f, " %u:%d", i, wav_scr.c[i]);
                        fprintf(f, "\n");
                        for (i = 0; i < WAV_N; i++) {
                            for (j = 0; j < WAV_NL; j++)
                                fprintf(f, "%d%c", ps->t[i * WAV_NL + j], j + 1u < WAV_NL ? ' ' : '\n');
                        }
                        fclose(f);
                    }
                }
            }

    /* 3. the presets */
    n = ENG_WAVELET.npresets;
    for (i = 0; i < n; i++) {
        const preset_t *pr = &ENG_WAVELET.presets[i];
        clock_t c0 = clock();
        result_t r = render_phrase(&ENG_WAVELET, pr, i % NPART, 1, 0);
        t_wav += (double)(clock() - c0) / CLOCKS_PER_SEC;
        n_wav += NBLK * CTL;
        printf("preset %2u %-10s %016llx peak %5d jump %5d free %3u\n", i, pr->name, (unsigned long long)r.h, r.peak,
               r.maxjump, r.free_at);
        snprintf(path, sizeof path, "preset %u %s: peak in range", i, pr->name);
        check(path, r.peak >= 250 && r.peak < 100000);   /* (three voices: ANALOG reaches 92000) */
        snprintf(path, sizeof path, "preset %u %s: voices free after the release", i, pr->name);
        check(path, r.free_at != 0);
        if (i < 12u) {
            snprintf(path, sizeof path, "%s/preset_%02u.wav", outdir, i);
            wav_write(path, wavbuf, NBLK * CTL);
        }
    }
    for (i = 0; i < ENG_ANALOG.npresets; i++) {
        clock_t c0 = clock();
        render_phrase(&ENG_ANALOG, &ENG_ANALOG.presets[i], i % NPART, 1, 0);
        t_ana += (double)(clock() - c0) / CLOCKS_PER_SEC;
        n_ana += NBLK * CTL;
    }
    printf("cost: WAVELET %.1f ns / sample, ANALOG %.1f ns / sample (this machine; the phrase's voices)\n",
           t_wav / n_wav * 1e9, t_ana / n_ana * 1e9);

    /* 4. a table switch while playing: no step */
    {
        uint32_t si = 0;
        for (i = 0; i < n; i++)                          /* a sine source (DB20): its own jumps are small */
            if (ENG_WAVELET.presets[i].e[0] == WW_SIN && ENG_WAVELET.presets[i].e[1] == 6)
                si = i;
        result_t a = render_phrase(&ENG_WAVELET, &ENG_WAVELET.presets[si], 0, 0, 0);
        result_t b = render_phrase(&ENG_WAVELET, &ENG_WAVELET.presets[si], 0, 0, 1);
        printf("cross-fade: steady jump %d, with a wave switch %d\n", a.maxjump, b.maxjump);
        check("a wave switch while playing shows no step (jump <= 1.5 x the sound's own)", b.maxjump <= a.maxjump * 3 / 2 + 64);
        snprintf(path, sizeof path, "%s/switch.wav", outdir);
        wav_write(path, wavbuf, NBLK * CTL);
    }

    /* 6. the render against a naive evaluation of the same table: no row cache, no integer shortcuts (a stale cached row
     *    once gave about ten wrong samples a second per voice, and nothing else in this file could see it) */
    {
        uint32_t note, wv, bad_all = 0, tested = 0;
        static const uint8_t NOTES[] = {0, 12, 36, 48, 60, 72, 84, 96, 120, 127};
        for (wv = 0; wv < 2u; wv++)
            for (note = 0; note < sizeof NOTES; note++) {
                track_t *t = &trk[0];
                voice_t *v = &t->v[0];
                int32_t out[CTL], g1[WAV_NB], gate[WAV_NB], lastmax = 0;
                vmod_t m;
                uint32_t blk, i, j, bad = 0, nb = 64;
                part_reset(t, E_WAV);
                t->p[P_E0] = (int16_t)(wv ? 2 : 0);          /* SAW, PLS: edges everywhere */
                t->p[P_E1] = (int16_t)(wv ? 0 : 2);          /* HAAR, DB4 */
                t->p[P_E2] = 17;
                t->p[P_E4] = 80;
                t->p[P_E5] = 40;
                t->p[P_E6] = 2;
                build_table(t);
                voice_on(t, &ENG_WAVELET, v, NOTES[note], 0);
                v->stage = 2;
                memset(&m, 0, sizeof m);
                m.pitch16 = (int32_t)NOTES[note] * 16;
                m.inc = pitch_inc((uint32_t)clamp(m.pitch16, 0, 2047));
                m.amp0 = m.amp1 = 32767;
                m.shape = 64 << 8;
                m.cutoff = 0;
                for (blk = 0; blk < nb; blk++) {
                    uint32_t ph0 = v->ph[0];
                    wav_part_t *ps = WAV_PART(t);
                    int32_t cutq8 = (clamp(t->p[P_E4], 0, 127) * 3072 + m.cutoff * 24) / 127;
                    memset(out, 0, sizeof out);
                    ENG_WAVELET.render(t, v, out, CTL, &m);
                    wav_gains(t->p, cutq8, m.inc, g1, gate);
                    if (blk < 2u)
                        continue;                            /* (the gains are steady from the second block) */
                    for (i = 0; i < CTL; i++) {
                        uint32_t ph = ph0 + i * m.inc, r = ph >> (32u - WAV_LOG2N);
                        double f = (double)((ph >> 8) & 0x7FFFu) / 32768.0, acc = 0;
                        int32_t y, want, d;
                        for (j = 0; j < WAV_NB; j++)
                            acc += (double)g1[j] * ((1.0 - f) * ps->t[r * WAV_NL + j] + f * ps->t[(r + 1u) * WAV_NL + j]);
                        y = (int32_t)(acc / 512.0);
                        want = voice_amp(soft_knee(y, 24000), &m, i);
                        d = out[i] - want;
                        if (d < 0)
                            d = -d;
                        if (d > 6)
                            bad++;
                        if (d > lastmax)
                            lastmax = d;
                    }
                }
                bad_all += bad;
                tested++;
                if (bad) {
                    snprintf(path, sizeof path, "render = naive evaluation (wave %u, note %u: %u wrong samples, worst %d)",
                             wv ? 2u : 0u, NOTES[note], bad, lastmax);
                    check(path, 0);
                }
            }
        printf("naive render check: %u notes x 2 waves, %u wrong samples\n", tested, bad_all);
    }

    /* 7. a table switch with the resonance up: the cross-fade's product of two Q15 numbers must not wrap */
    {
        preset_t pr = {"RESO XF", {0, 0, 0, 100, 70, 127, 2, 0}, {0, 60, 100, 60}, 0, 0, {0}, 0};
        pr.e[7] = 90;                                       /* with the window too: the dry cycle crosses over as well */
        result_t a = render_phrase(&ENG_WAVELET, &pr, 0, 0, 0);
        result_t b = render_phrase(&ENG_WAVELET, &pr, 0, 0, 2);   /* the phase flips: the tables differ most */
        printf("cross-fade at RES 127 + WNDW: steady jump %d, switch %d (peaks %d, %d)\n", a.maxjump, b.maxjump, a.peak, b.peak);
        check("a switch at RES 127 shows no wrapped samples (jump <= 1.5 x steady, peak <= 1.5 x steady)",
              b.maxjump <= a.maxjump * 3 / 2 + 64 && b.peak <= a.peak * 3 / 2 + 64);
    }

    /* 7b. the cross-fade's arithmetic on tables that differ as much as they can: +A everywhere against -A everywhere */
    {
        track_t *t = &trk[2];
        voice_t *v = &t->v[0];
        wav_part_t *ps;
        int32_t out[CTL], g1[WAV_NB], gate[WAV_NB], worst = 0;
        double sumg = 0;
        vmod_t m;
        uint32_t i, j, pi = 2, bad = 0;
        part_reset(t, E_WAV);
        t->p[P_E4] = 127;
        t->p[P_E5] = 100;
        build_table(t);
        ps = WAV_PART(t);
        for (i = 0; i < WAV_TSIZE; i++) {
            ps->t[i] = 16000;
            wav_scr.t[i] = -16000;
        }
        wav_scr.owner = (uint8_t)(pi + 1u);
        wav_scr.stage = WS_XF;
        wav_xf[pi] = 8;                                     /* (WAV_XF - 8) / WAV_XF of the way: the share starts at 1/2 */
        voice_on(t, &ENG_WAVELET, v, 48, 0);
        memset(&m, 0, sizeof m);
        m.pitch16 = 48 * 16;
        m.inc = pitch_inc(48 * 16);
        m.amp0 = m.amp1 = 32767;
        m.shape = 64 << 8;
        memset(out, 0, sizeof out);
        ENG_WAVELET.render(t, v, out, CTL, &m);
        wav_gains(t->p, (127 * 3072) / 127, m.inc, g1, gate);
        for (j = 0; j < WAV_NB; j++)
            sumg += g1[j];
        for (i = 0; i < CTL; i++) {
            double xw = (16384.0 + 64.0 * i) / 32768.0, y = 16000.0 * sumg / 512.0, yx = y + (-y - y) * xw;
            int32_t want = voice_amp(soft_knee((int32_t)yx, 24000), &m, i), d = out[i] - want;
            d = d < 0 ? -d : d;
            if (d > 6)
                bad++;
            if (d > worst)
                worst = d;
        }
        wav_scr.owner = 0;
        wav_xf[pi] = 0;
        printf("cross-fade arithmetic: %u wrong samples of %d, worst %d (sum of gains %.0f Q10)\n", bad, CTL, worst, sumg);
        check("the cross-fade of opposite tables matches the double-precision value (no wrap)", bad == 0 && sumg > 2048);
    }

    /* 8. a part that leaves WAVELET and comes back plays nothing until its table is built again (no stale table) */
    {
        track_t *t = &trk[1];
        int32_t out[CTL], last[NVOICE] = {0};
        uint32_t b, loud_before = 0, loud_after = 0;
        part_reset(t, E_WAV);
        t->p[P_E4] = 100;
        build_table(t);
        voice_on(t, &ENG_WAVELET, &t->v[0], 48, 0);
        for (b = 0; b < 20u; b++) {
            uint32_t i;
            render_block(t, out, b, last, 0);
            for (i = 0; i < CTL; i++)
                loud_before += out[i] > 100 || out[i] < -100;
        }
        t->engine = 0;                                      /* a visit of ANALOG: wav_tick sees a part that is not WAVELET */
        wav_tick();
        t->engine = (uint8_t)E_WAV;
        check("the table is gone after a visit of another engine", !wav_valid[1]);
        {
            int32_t o2[CTL];
            memset(o2, 0, sizeof o2);
            t->v[0].s[0] = 0;
            ENG_WAVELET.render(t, &t->v[0], o2, CTL, &(vmod_t){.pitch16 = 48 * 16, .inc = pitch_inc(48 * 16), .amp0 = 32767,
                                                              .amp1 = 32767, .shape = 64 << 8});
            for (b = 0; b < CTL; b++)
                loud_after += o2[b] != 0;
        }
        check("a part with no table is silent, not stale", loud_before > 100 && loud_after == 0);
    }

    /* 5. the anti-aliasing gates */
    {
        int32_t g[WAV_NB], gt[WAV_NB];
        int16_t p[P_COUNT] = {0};
        p[P_E6] = 2;
        wav_gains(p, 12 << 8, pitch_inc(2047), g, gt);     /* ~12.5 kHz: 1.7 harmonics fit */
        check("top pitch: band 1 on", g[0] >= 1000);
        check("top pitch: band 2 (harmonics 1..2) partly", g[1] > 0 && g[1] < 1024);
        check("top pitch: bands 3.. off", g[2] == 0 && g[8] == 0);
        wav_gains(p, 12 << 8, pitch_inc(69 * 16), g, gt);  /* A4: 50 harmonics fit */
        check("A4: bands 1..6 on", g[0] >= 1000 && g[5] >= 1000);
        check("A4: band 7 (32..64) partly", g[6] > 0 && g[6] < 1024);
        check("A4: bands 8, 9 off", g[7] == 0 && g[8] == 0);
        wav_gains(p, 4 << 8, pitch_inc(69 * 16), g, gt);   /* the filter at band 4, LP 24 dB / band */
        check("LP: band 4 at -4 dB (slope x softplus 0)", g[3] > 500 && g[3] < 800);
        check("LP: band 6 well below", g[5] < g[3] / 8);
        p[P_E5] = 127;
        wav_gains(p, 4 << 8, pitch_inc(69 * 16), g, gt);
        check("RES: +20 dB at the cutoff (24 - 4)", g[3] > 8000 && g[3] < 12000);
    }
    printf("%s (%d failures)\n", fails ? "FAILED" : "OK", fails);
    return fails != 0;
}
