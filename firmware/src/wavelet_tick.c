/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Pierre Arnaud GUYOT (OktoPierre) */
/* WAVELET: the builds of the parts' tables and the switches, once per audio block (eng_wavelet.c explains;
 * included by engines.c after ENGINES[], used by fx.c mix_block on core 0 before the parts render). */

static int wav_want(const track_t *t, int16_t *w)        /* E0 .. E3 of a part, 1 = differ from w (and set) */
{
    int d = 0;
    uint32_t i;
    for (i = 0; i < 4u; i++) {
        int16_t v = t->p[P_E0 + i];
        if (i == 0u) v = (int16_t)clamp(v, 0, 5);
        else if (i == 1u) v = (int16_t)clamp(v, 0, WAV_NW - 1);
        else v = (int16_t)clamp(v, 0, 127);
        if (w[i] != v) {
            w[i] = v;
            d = 1;
        }
    }
    return d;
}

static void wav_copy_table(wav_part_t *ps, const wav_scratch_t *s)
{
    const uint32_t *src = (const uint32_t *)(const void *)s->t;
    uint32_t *dst = (uint32_t *)(void *)ps->t, i;
    for (i = 0; i < WAV_TSIZE / 2u; i++)
        dst[i] = src[i];
    for (i = 0; i < 4u; i++)
        ps->snap[i] = s->snap[i];
    ps->valid = 1;
    ps->xf = 0;
}

/* once per audio block, on core 0 before the parts render (fx.c mix_block): the builds and the switches */
static void wav_tick(void)
{
    wav_scratch_t *s = &wav_scr;
    uint32_t p;
    for (p = 0; p < NPART; p++) {
        track_t *t = &trk[p];
        wav_part_t *ps;
        int16_t want[4];
        if (ENGINES[eng_idx(t->engine)] != &ENG_WAVELET) {
            if (s->owner == p + 1u)                     /* it left: the scratch is free */
                s->owner = 0;
            continue;
        }
        ps = WAV_PART(t);
        if (s->owner == p + 1u) {
            if (s->stage == WS_XF) {
                if (ps->xf > 1u)
                    ps->xf--;
                else {                                  /* the fade is over: the new table takes the place */
                    wav_copy_table(ps, s);
                    s->owner = 0;
                }
                continue;
            }
            want[0] = s->snap[0]; want[1] = s->snap[1]; want[2] = s->snap[2]; want[3] = s->snap[3];
            if (wav_want(t, want)) {                    /* moved again: start over */
                uint32_t i;
                for (i = 0; i < 4u; i++)
                    s->snap[i] = want[i];
                s->stage = WS_SRC;
            }
            wav_build_step(s, WAV_QUANTUM);
            if (s->stage == WS_DONE) {
                if (!ps->valid) {                       /* nothing to fade from */
                    wav_copy_table(ps, s);
                    s->owner = 0;
                } else {
                    s->stage = WS_XF;
                    ps->xf = WAV_XF;
                }
            }
        } else if (!s->owner) {
            want[0] = ps->snap[0]; want[1] = ps->snap[1]; want[2] = ps->snap[2]; want[3] = ps->snap[3];
            if (wav_want(t, want) || !ps->valid) {
                uint32_t i;
                for (i = 0; i < 4u; i++)
                    s->snap[i] = want[i];
                s->owner = (uint8_t)(p + 1u);
                s->stage = WS_SRC;
                s->macs = 0;
            }
        }
    }
}
