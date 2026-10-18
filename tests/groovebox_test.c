/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Pierre Arnaud GUYOT (OktoPierre) */
/* OKTOWAVE GROOVEBOX (this branch): the engines one can pick are WAVELET and DRUM; every other engine stays in the firmware
 * and plays what a project or a user preset names. Built with the instrument's own define, on the firmware's sources as the
 * host tests build them (tests/hostsim.c's includes, without its fork):
 *   cc -w -DOKTOWAVE_GROOVEBOX=1 -DOKTOWAVE_BOOT=1 -Ibuild/gen -Ifirmware/src -o build/host/groovebox_test tests/groovebox_test.c -lm
 * Checks: the shown list and its order, what eng_ok / eng_valid / eng_rank / eng_step say, that a hidden engine still renders
 * (a project of Felucca with an ANALOG track keeps its sound) and that the power-on tracks are the WAVELET ones. */
#include <stdio.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifndef OKTOWAVE_GROOVEBOX
#error "build tests/groovebox_test.c with -DOKTOWAVE_GROOVEBOX=1 (the instrument's define)"
#endif
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

static int bad;
static void check(const char *what, int ok)
{
    printf("%-100s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        bad++;
}

int main(void)
{
    uint32_t e, n, shown = 0;
    check("two engines are shown: WAVELET, then DRUM", NENG_SHOWN == 2u && eng_vis(0) == ENGI_WAVELET && eng_vis(1) == ENGI_DRUM);
    for (e = 0; e < NENGINES; e++)
        shown += (uint32_t)eng_ok(e);
    check("eng_ok is true for those two and no other", shown == 2u && eng_ok(ENGI_WAVELET) && eng_ok(ENGI_DRUM) && !eng_ok(0) &&
          !eng_ok(ENGI_FM6) && !eng_ok(13) && !eng_ok(NENGINES));
    check("eng_valid still accepts every engine that plays (ANALOG, FM6, SLICE, DRUM, WAVELET), not the retired one",
          eng_valid(0) && eng_valid(ENGI_FM6) && eng_valid(13) && eng_valid(ENGI_DRUM) && eng_valid(ENGI_WAVELET) && !eng_valid(ENGI_DIGITAL) &&
          !eng_valid(NENGINES));
    check("a hidden engine is shown as the first (eng_rank), the others at their place",
          eng_rank(0) == 0u && eng_rank(ENGI_FM6) == 0u && eng_rank(ENGI_WAVELET) == 0u && eng_rank(ENGI_DRUM) == 1u);
    check("eng_step wraps over the two both ways, from a hidden engine too",
          eng_step(ENGI_WAVELET, 1) == ENGI_DRUM && eng_step(ENGI_DRUM, 1) == ENGI_WAVELET && eng_step(ENGI_WAVELET, -1) == ENGI_DRUM &&
          eng_step(ENGI_DRUM, -1) == ENGI_WAVELET && eng_step(0, 1) == ENGI_DRUM && eng_step(0, -1) == ENGI_DRUM);
    check("the power-on tracks and the fallback of old projects are the WAVELET ones, the drums last",
          TRK_DEF[0][0] == ENGI_WAVELET && TRK_DEF[1][0] == ENGI_WAVELET && TRK_DEF[2][0] == ENGI_WAVELET && TRK_DEF[3][0] == ENGI_DRUM &&
          BOOT_DEF[1][1] == WP_WARM_PAD);
    check("WAVELET has its presets and their category letters agree in number", ENG_WAVELET.npresets == str_len(WAVELET_PRESET_CAT) &&
          ENG_WAVELET.npresets >= 60u);
    for (n = 0; n < ENG_WAVELET.npresets && n < 15u; n++)
        if (str_len(ENG_WAVELET.presets[n].name) > 10u || !ENG_WAVELET.presets[n].name[0])
            break;
    check("the first presets (the TOP Sounds) have names of at most ten characters", n == 15u || n == ENG_WAVELET.npresets);
    {   /* a hidden engine still plays: ANALOG's SAW LEAD on a track renders a sound */
        track_t *t = &trk[0];
        int32_t out[CTL], peak = 0;
        uint32_t i, b;
        memset(t, 0, sizeof *t);
        t->engine = t->eng_req = 0;
        for (i = 0; i < P_COUNT; i++)
            t->p[i] = TP[i].def;
        for (i = 0; i < 8u; i++)
            t->p[P_E0 + i] = ENG_ANALOG.presets[0].e[i];
        t->p[P_ATK] = ENG_ANALOG.presets[0].env[0];
        t->p[P_REL] = ENG_ANALOG.presets[0].env[3];
        t->v[0].note = 60; t->v[0].vel = 100; t->v[0].gate = 1; t->v[0].active = 1; t->v[0].stage = 1;
        t->v[0].pitch16 = t->v[0].pitch_cur = 60 * 16;
        ENG_ANALOG.note_on(t, &t->v[0]);
        for (b = 0; b < 40u; b++) {
            vmod_t m;
            memset(&m, 0, sizeof m);
            m.pitch16 = 60 * 16;
            m.inc = pitch_inc(60 * 16);
            m.amp0 = b ? 20000 : 0;
            m.amp1 = 20000;
            m.shape = 64 << 8;
            memset(out, 0, sizeof out);
            ENGINES[0]->render(t, &t->v[0], out, CTL, &m);
            for (i = 0; i < CTL; i++)
                if (out[i] > peak || -out[i] > peak)
                    peak = out[i] < 0 ? -out[i] : out[i];
        }
        check("a track left on ANALOG (a project of Felucca) still sounds", peak > 1000);
    }
    printf("%s (%d failures)\n", bad ? "FAILED" : "OK", bad);
    return bad != 0;
}
