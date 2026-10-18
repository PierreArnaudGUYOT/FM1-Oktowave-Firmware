#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Pierre Arnaud GUYOT (OktoPierre)
// Exports the presets this firmware can play from the Wavelet Synthesizer's tools/spec.js (beta 0.7: the factory order,
// OktoPierre's 22 TOP Sounds first) to tools/wavelet_presets_src.json: every engine-A preset ("A · ") and the A/B presets
// that are mostly A (ab_mix <= 0.5, played as their A part); engine-B presets are left out (no engine B on the FM-1).
// Each entry carries `top` = its place among the TOP Sounds (0 .. 21), absent for the others.
//   node tools/export_wavelet_presets.js "C:/path/to/wavelet synthesizer"
'use strict';
const fs = require('fs');
const path = require('path');
const repo = process.argv[2];
if (!repo) { console.error('usage: node tools/export_wavelet_presets.js PATH_TO_WAVELET_SYNTHESIZER_REPO'); process.exit(2); }
const S = require(path.join(repo, 'tools', 'spec.js'));
const keep = p => /^A · /.test(p.name) || (/^AB · /.test(p.name) && (p.v.ab_mix === undefined ? 0 : p.v.ab_mix) <= 0.5);
const out = {
  source: `Wavelet Synthesizer ${S.APP.version} tools/spec.js (engine-A presets), GPL-3.0-or-later, Pierre Arnaud GUYOT`,
  defaults: Object.fromEntries(S.PARAMS.map(p => [p.key, p.def])),
  topCount: S.TOP,
  categories: S.CATEGORIES,
  presets: S.PRESETS.map((p, i) => ({ p, top: i < S.TOP ? i : undefined })).filter(x => keep(x.p)).map(x => ({
    name: x.p.name, cat: x.p.cat, stars: x.p.stars, rank: x.p.rank, top: x.top, v: x.p.v })),
};
fs.writeFileSync(path.join(__dirname, 'wavelet_presets_src.json'), JSON.stringify(out, null, 1));
console.log(`${out.presets.length} presets, ${out.presets.filter(p => p.top !== undefined).length} of the ${S.TOP} TOP Sounds`);
