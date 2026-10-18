/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Pierre Arnaud GUYOT (OktoPierre) */
/* The shared voice budget with OKTOWAVE's weights (voice.c voice_weight): a WAVELET voice counts two of the NVOICE = 8 units, every other engine one.
 * On the firmware's sources as the host tests build them (no fork):
 *   cc -w -Ibuild/gen -Ifirmware/src -o build/host/budget_test tests/budget_test.c -lm
 * Checks: the count in units; that a part of any other engine behaves as before (one voice taken when the budget is full); that a WAVELET voice takes
 * two units (two voices of other engines, or one WAVELET voice) when it needs room; that the voices of one WAVELET part never exceed three. */
#include <stdio.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define __attribute__(x)
#define memset felucca_memset
#define memcpy felucca_memcpy
#define memcmp felucca_memcmp
#include "felucca_tables.h"
#include "../firmware/src/libc.c"
#undef memset
#undef memcpy
#undef memcmp
static struct { volatile uint32_t notes, buttons; } fm1_in;
#include "../firmware/src/core.h"
#include "../firmware/src/engines.c"
#include "../firmware/src/params.c"
#include "../firmware/src/mod.c"
#include "../firmware/src/voice.c"
#include "../firmware/src/slicer.c"
#include "../firmware/src/fx.c"

static void events_block(uint32_t n) { (void)n; }       /* seq.c's (this test mixes nothing, but gcc at -O0 keeps mix_block) */

static int bad;
static void check(const char *what, int ok)
{
    printf("%-110s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        bad++;
}

static void reset(void)
{
    uint32_t p, i;
    memset(trk, 0, sizeof trk);
    for (p = 0; p < NPART; p++) {
        trk[p].engine = trk[p].eng_req = 0;            /* ANALOG */
        for (i = 0; i < P_COUNT; i++)
            trk[p].p[i] = TP[i].def;
        trk[p].p[P_VOICE] = V_POLY;
    }
}
static void sound(uint32_t part, uint32_t n, uint32_t base_age)    /* n active voices on the part, ages base_age .. */
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        voice_t *v = &trk[part].v[i];
        v->active = 1; v->gate = 1; v->stage = 2; v->env = 1 << 23; v->note = (uint8_t)(48 + i + 12 * part); v->age = base_age + i;
    }
}

int main(void)
{
    track_t *w = &trk[0];
    uint32_t i, n;
    reset();
    trk[0].engine = trk[0].eng_req = ENGI_WAVELET;
    sound(0, 3, 10);
    sound(1, 2, 20);
    check("the count is in units: 3 WAVELET voices (2 each) and 2 ANALOG voices (1 each) = 8", voices_busy() == 8u);
    check("WAVELET has a cap of three voices per part (engine_t.poly)", ENG_WAVELET.poly == 3u && trk_nvoice(&trk[0]) == 3u);

    /* an ANALOG part asks for room while the budget is full: one voice is taken, as before */
    reset();
    sound(1, 8, 10);
    check("(setup) eight ANALOG voices fill the budget", voices_busy() == 8u);
    voice_room(&trk[2], 0);
    check("a part of another engine needing room: one voice is given up, the budget has room for one", voices_busy() == 7u);

    /* a WAVELET part asks for room: two units are freed */
    reset();
    trk[0].engine = trk[0].eng_req = ENGI_WAVELET;
    sound(1, 8, 10);
    voice_room(&trk[0], 0);
    check("a WAVELET part needing room with the budget full: two voices of other engines are given up", voices_busy() == 6u);
    reset();
    trk[0].engine = trk[0].eng_req = ENGI_WAVELET;
    trk[1].engine = trk[1].eng_req = ENGI_WAVELET;
    sound(1, 3, 10);
    sound(2, 2, 20);
    voice_room(&trk[0], 0);
    check("a WAVELET part needing room: one WAVELET voice (two units) is given up when that frees enough", voices_busy() + 2u <= NVOICE);

    /* voice_alloc: the part's voices stay within the cap; a fourth note takes the oldest released, else the oldest held but the lowest */
    reset();
    trk[0].engine = trk[0].eng_req = ENGI_WAVELET;
    sound(0, 3, 10);
    {
        voice_t *v = voice_alloc(&trk[0], 90);
        check("a fourth note on a WAVELET part takes one of its three voices (the cap), not a fourth", v >= &trk[0].v[0] && v < &trk[0].v[3]);
    }
    (void)w; (void)i; (void)n;
    printf("%s (%d failures)\n", bad ? "FAILED" : "OK", bad);
    return bad != 0;
}
