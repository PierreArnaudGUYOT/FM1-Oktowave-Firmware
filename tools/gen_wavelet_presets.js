#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Pierre Arnaud GUYOT (OktoPierre)
// The WAVELET engine's presets (firmware/src/wavelet_presets.h) from the engine-A presets of the Wavelet
// Synthesizer (tools/wavelet_presets_src.json: its beta 0.6 factory list, TOP Sounds first, then by category).
// Each sound's parameters are mapped onto the eight engine values, the ADSR, the ENV > FLT amount, MONO and
// the four sends; what the FM-1 engine has not (LFO settings, unison, the sub oscillator, LFO 2, the morph)
// is left out, so some sounds are plainer than in the VST.
//   node tools/gen_wavelet_presets.js
'use strict';
const fs = require('fs');
const path = require('path');
const src = JSON.parse(fs.readFileSync(path.join(__dirname, 'wavelet_presets_src.json'), 'utf8'));
const DEF = src.defaults;
const SLOPES = [12, 18, 24, 36, 48, 96];
const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
const r = Math.round;
const msv = s => clamp(r(127 * Math.log10(clamp(s * 1000, 1, 10000)) / 4), 0, 127);   // seconds -> Felucca's 0..127

// the short names (at most 10 characters) of the sounds kept; a sound not here is left out (the "Shift" variants of
// the Source Phase section need LFO 2 moving the phase, which the FM-1 engine has not)
const NAMES = {
  'Noise Wash Pad': 'NOISE WASH', 'Siren Sweep': 'SIREN SWP', 'Wavelet Lead': 'WAVLT LEAD', 'Wave Brass': 'WAVE BRASS',
  'Wave Riser': 'WAVE RISER', 'Warm Pad': 'WARM PAD', 'Wave Attack Lead': 'ATTACK LD', 'Dub Echo Lead': 'DUB ECHO',
  'Wave Pulse Lead': 'PULSE LEAD', 'Cello Section': 'CELLOS', 'Spiccato Strings': 'SPICCATO', 'Shimmering Strings': 'SHIMMER ST',
  'Wave Robot': 'WAVE ROBOT', 'Laser Sweep': 'LASER SWP', 'Wave Wobble Bass': 'WOBBLE BS', 'Reese Sub': 'REESE SUB',
  'Acid Sub': 'ACID SUB', 'Ladder Bass': 'LADDER BS', 'Wave S/H Lead': 'S/H LEAD', 'Chorus Square Lead': 'CHORUS SQR',
  'Saw Lead': 'SAW LEAD', 'Whistle Lead': 'WHISTLE', 'Clean Keys': 'CLEAN KEYS', 'Tine Piano': 'TINE PIANO', 'Clavi': 'CLAVI',
  'Lo-Fi Piano': 'LOFI PIANO', 'Poly Chorus Pad': 'CHORUS PAD', 'Analog Brass Pad': 'BRASS PAD', 'Wave Morph Pad': 'MORPH PAD',
  'Glass Pad': 'GLASS PAD', 'Sweep Pad': 'SWEEP PAD', 'Analog Pad': 'ANALOG PAD', 'String Machine': 'STRING MCH',
  'Ensemble Strings': 'ENSEMBLE', 'Wave Tremolo Strings': 'TREMOLO ST', 'Solina Cellos': 'SOLINA', 'Pizzicato': 'PIZZICATO',
  'Slow Strings': 'SLOW STR', 'French Horn': 'FR HORN', 'Noise Brass': 'NOISE BRS', 'Brass Section Stab': 'BRASS STAB',
  'Steel Pluck': 'STEEL PLCK', 'Harp': 'HARP', 'Synth Pluck': 'SYN PLUCK', 'UFO': 'UFO', 'Wave Atom Scanner': 'ATOM SCAN',
  'Wind': 'WIND', 'Alarm': 'ALARM', 'Noise Snare': 'NOISE SNR', 'Closed Hat': 'CLOSED HAT', 'Open Hat': 'OPEN HAT',
  'Synth Tom': 'SYNTH TOM', 'Init': 'INIT',
  'Window Phase Lead': 'WINDOW LD', 'Phase Clarinet': 'CLARINET', 'Phase Wobble Bass': 'PH WOBBLE', 'Pipe Phase': 'PIPE PHASE',
  'Phase Drift Pad': 'DRIFT PAD', 'Dawn Phase': 'DAWN PHASE', 'Noise Phase Pad': 'NOISE PH', 'Phase Glass Pad': 'PH GLASS',
  'Dark Phase Pad': 'DARK PHASE', 'Phase Ensemble': 'PH ENSMBLE', 'Phase Tremolo Strings': 'PH TREMOLO', 'Shifted Horns': 'SHIFT HORN',
  'Phase Stab': 'PHASE STAB', 'Phase Scanner': 'PH SCANNER', 'Phase Glitch': 'PH GLITCH',
  'Silk Strings': 'SILK STR',
  // engine B (additive): the B presets and the A/B ones that are mostly B
  'Mirror Sub Bass': 'MIRROR SUB', 'Theatre Organ': 'THEATRE OR', 'Space Choir': 'SPACE CHR', 'Focus Sweep': 'FOCUS SWP',
  'Flute Lead': 'FLUTE LEAD', 'Choir Pad': 'CHOIR PAD', 'Reed Piano': 'REED PIANO', 'Toy Piano': 'TOY PIANO',
  'Full Drawbar Organ': 'FULL ORGAN', 'Reed Organ': 'REED ORGAN', 'Octave Organ': 'OCT ORGAN', 'Distorted Organ': 'DIST ORGAN',
  'Ensemble Organ': 'ENS ORGAN', 'Jazz Organ': 'JAZZ ORGAN', 'Gospel Organ': 'GOSPEL ORG', 'Church Organ': 'CHURCH ORG',
  'Flute Organ': 'FLUTE ORG', 'Random Glass': 'RAND GLASS', 'Mirror Pad': 'MIRROR PAD', 'Glass Ensemble': 'GLASS ENS',
  'Halo Pad': 'HALO PAD', 'Vox Strings': 'VOX STRNGS', 'Chorus Flute': 'CHRS FLUTE', 'Clarinet': 'REED CLAR',
  'Xylophone': 'XYLOPHONE', 'Music Box': 'MUSIC BOX', 'Glockenspiel': 'GLOCKEN', 'Vibraphone': 'VIBES', 'Marimba': 'MARIMBA',
  'Tubular Bells': 'TUBE BELLS', 'Alien Texture': 'ALIEN TXT', 'Sci-Fi Burble': 'SCIFI BRBL', 'Wood Block': 'WOOD BLOCK',
  'Glass Choir': 'GLASS CHR', 'Glass Keys': 'GLASS KEYS', 'Wavelet Choir': 'WAVLT CHR', 'Wave Atom Choir': 'ATOM CHOIR',
  'Crystal Bell': 'CRYST BELL',
};
const isB = p => /^B · /.test(p.name) || (/^AB · /.test(p.name) && (p.v.ab_mix === undefined ? 0 : p.v.ab_mix) > 0.5);
function shortName(name) {
  const n = name.replace(/^(A|B|AB) · /, '').trim();
  if (n === 'Init' && /^B · /.test(name)) return 'INIT B';
  return NAMES[n] || null;
}

// the bars of a B preset as the FM-1's engine B gets them: the nine bars (0 .. 1, to 0.05); a positive tilt (the engine only has tilts down to 0) is
// baked into the bars, scaled to a largest of 1
const barsOf = p => {
  const v = Object.assign({}, DEF, p.v);
  let o = [];
  for (let i = 1; i <= 9; i++) o.push(Math.min(1, Math.max(0, v['b_bar' + i])));
  if (v.b_tilt > 0) {
    o = o.map((x, j) => x * Math.pow(10, v.b_tilt * j / 20));
    const m = Math.max(...o);
    o = o.map(x => x / m);
  }
  return o.map(x => Math.round(x * 20) / 20);
};
const REG = [];                                          // the 16 registrations (nine bars, 0 .. 1): the first is flat (BARS 0), the others k-medoids of the presets' bars
{
  const items = src.presets.filter(p => shortName(p.name) && isB(p)).map(barsOf);
  const flat = [1, 1, 1, 1, 1, 1, 1, 1, 1];
  const d2 = (x, y) => x.reduce((s2, v, j) => s2 + (v - y[j]) ** 2, 0);
  const med = [flat];
  for (let n = 1; n < 16; n++) {                        // farthest-first start (items not yet within 0.01 of a medoid)
    let bi = -1, bd = 0.01;
    items.forEach((it, i) => { const d = Math.min(...med.map(m => d2(it, m))); if (d > bd) { bd = d; bi = i; } });
    if (bi < 0) break;
    med.push(items[bi].slice());
  }
  for (let it = 0; it < 30; it++) {                     // k-medoids: each cluster's member with the least total error (the flat one stays)
    const cl = med.map(() => []);
    items.forEach(x => { let k = 0, bd = 1e9; med.forEach((m, i) => { const d = d2(x, m); if (d < bd - 1e-12) { bd = d; k = i; } }); cl[k].push(x); });
    let moved = false;
    for (let k = 1; k < med.length; k++) {
      if (!cl[k].length) continue;
      let best = med[k], be = 1e9;
      cl[k].forEach(c => { const e = cl[k].reduce((s2, x) => s2 + d2(x, c), 0); if (e < be - 1e-12) { be = e; best = c; } });
      if (best !== med[k]) { med[k] = best; moved = true; }
    }
    if (!moved) break;
  }
  for (const m of med) REG.push(m);
  // canonical registrations fill what is left: a saw (1/n), a square (odd only), the fundamental alone, the octaves
  const filler = [[1, .5, .33, .25, .2, .17, .14, .12, .11], [1, 0, .33, 0, .2, 0, .14, 0, .11], [1, 0, 0, 0, 0, 0, 0, 0, 0],
    [1, 1, 0, 1, 0, 0, 0, 1, 0], [1, 0, 0, 0, 1, 0, 0, 0, 0], [0, 1, 1, 1, 1, 1, 1, 1, 1], [1, .7, .5, .35, .25, .17, .12, .08, .06]];
  for (const f of filler) if (REG.length < 16) REG.push(f);
}
const regOf = bars => { let bi = 0, bd = 1e9; REG.forEach((rg, i) => { const d = rg.reduce((s2, x, j) => s2 + (x - bars[j]) ** 2, 0); if (d < bd - 1e-9) { bd = d; bi = i; } }); return bi; };
const out = ['/* SPDX-License-Identifier: GPL-3.0-only */', '/* generated by tools/gen_wavelet_presets.js from tools/wavelet_presets_src.json: do not edit.',
  ' * The presets of the Wavelet Synthesizer (Pierre Arnaud GUYOT, GPL-3.0-or-later), mapped onto the FM-1 engine, A or B:',
  ' * {WAVE WVLT PHAS/WDTH/BARS SPRS/TILT CUT RES FLT WNDW/FADE}, {A D S R}, ENV > FLT, MONO, FX(sends) */',
  '/* engine B (WAVE 6 .. 11): the 16 registrations of the nine bands, Q10 (1024 = 1.0): BARS = E2 / 8 */',
  `static const uint16_t WAV_REG[16][9] = {${REG.map(rg => '{' + rg.map(x => Math.round(x * 1024)).join(', ') + '}').join(', ')}};`, '',
  'static const preset_t WAVELET_PRESETS[] = {'];
const seen = new Set();
let cat = null, kept = 0;
const ordered = src.presets.filter(p => p.top !== undefined).sort((a, b) => a.top - b.top)
  .concat(src.presets.filter(p => p.top === undefined));
let idx = 0;
const defs = [];
const letters = [];
// the FM-1's categories (category.c: B BASS, L LEAD, P PAD, U PLUCK, K KEYS, D DRUM, F FX, O OTHER) from the VST's
const LETTER = { 'Bass': 'B', 'Lead': 'L', 'Keys': 'K', 'Organ': 'K', 'Pad': 'P', 'Strings': 'P', 'Brass & Wind': 'L', 'Pluck': 'U',
  'Bells & Mallets': 'U', 'FX': 'F', 'Drums & Perc': 'D', 'Init': 'L' };   // (Init: LEAD, so that OTHER stays empty as in Felucca)
function letterOf(p) {
  if (p.cat !== 'Source Phase') return LETTER[p.cat] || 'O';
  const n = p.name;
  return /Bass/.test(n) ? 'B' : /Lead|Clarinet|Horn|Stab/.test(n) ? 'L' : /Scanner|Glitch/.test(n) ? 'F' : 'P';
}
for (const p of ordered) {
  if (!shortName(p.name)) continue;
  kept++;
  const v = Object.assign({}, DEF, p.v);
  const B = isB(p);
  const wave = B ? 6 + clamp(r(v.b_pattern), 0, 5) : r(v.a_wave), wl = r(v.wavelet);
  let type = r(v.f_type); if (type === 3) type = 2;   // band-reject (unused by the presets) would be band-pass
  let si = 0, best = 1e9;
  SLOPES.forEach((s, i) => { const d = Math.abs(Math.log(s / v.f_slope)); if (d < best) { best = d; si = i; } });
  let e;
  if (B) {   // engine B: BARS (the nearest registration x 8), TILT (0 .. -24 dB per band over 0 .. 127), FADE (the upper bands fall to half)
    const tilt = v.b_tilt <= 0 ? r(-v.b_tilt / 24 * 127) : 0;   // (a positive tilt is in the bars: barsOf)
    const fade = v.b_sus >= 0.95 || v.b_bright >= 10 ? 0 : clamp(127 - msv(v.b_bright), 1, 127);
    e = [wave, wl, regOf(barsOf(p)) * 8, tilt, r(v.f_cutoff / 12 * 127), r(v.f_res / 24 * 127), type * 6 + si, fade].map(x => clamp(x, 0, 127));
  } else {
    const e2 = wave === 2 || wave === 3 ? r((v.a_width - 0.02) / 0.96 * 127) : (r(v.a_phase * 128) % 128 + 128) % 128;
    e = [wave, wl, clamp(e2, 0, 127), r(v.a_sparsity * 127), r(v.f_cutoff / 12 * 127), r(v.f_res / 24 * 127),
      type * 6 + si, r(v.p_depth * 127)].map(x => clamp(x, 0, 127));
  }
  // (engine B's scale envelope swells over the amp attack: the slower of the two attacks is kept)
  const env = [msv(B ? Math.max(v.amp_a, v.b_atk) : v.amp_a), msv(v.amp_d), clamp(r(v.amp_s * 127), 0, 127), msv(v.amp_r)];
  const fenv = clamp(r(v.f_env * 64 / 12), -64, 63);
  const mono = r(v.v_voices) <= 1 ? 1 : 0;
  const fx = [v.fx_drive, v.ch_mix, v.fx_dmix, v.fx_rmix].map(x => clamp(r(x * 127), 0, 127));
  const name = shortName(p.name);
  if (seen.has(name) || name.length > 10) throw new Error('name ' + name);
  seen.add(name);
  const c = (p.cat || '') + (p.stars ? ' ★' + p.stars : '');
  const catLabel = p.top !== undefined ? 'TOP Sounds' : p.cat;
  if (catLabel !== cat) { cat = catLabel; out.push(`    /* ${cat} */`); }
  letters.push(letterOf(p));
  defs.push(`#define WP_${name.replace(/[^A-Z0-9]+/g, '_')} ${idx++}`);
  out.push(`    {"${name}", {${e.join(', ')}}, {${env.join(', ')}}, ${fenv}, ${mono}, FX(${fx.join(', ')})},` +
    `   /* ${p.name.replace(/^(A|B|AB) · /, '')}${B ? ' (B)' : ''}${p.stars ? ' *' + p.stars : ''}${p.top !== undefined ? ' TOP ' + (p.top + 1) : ''} */`);
}
out.push('};', '', `#define WAVELET_PRESET_CAT "${letters.join('')}"   /* category.c: one letter per preset */`, '', '/* the presets by name, for the power-on sounds (engines.c TRK_DEF) */', ...defs, '');
fs.writeFileSync(path.join(__dirname, '..', 'firmware', 'src', 'wavelet_presets.h'), out.join('\n'));
// the manual's list (docs/manual.html, between <!-- PRESETS --> and <!-- /PRESETS -->)
const man = path.join(__dirname, '..', 'docs', 'manual.html');
if (fs.existsSync(man)) {
  const h = fs.readFileSync(man, 'utf8');
  const rows = [];
  let c2 = null;
  for (const p of ordered) {
    const name = shortName(p.name);
    if (!name) continue;
    const cl = p.top !== undefined ? 'TOP Sounds (the Wavelet Synthesizer favourites, best first)' : p.cat;
    if (cl !== c2) { c2 = cl; rows.push(`<tr><th colspan="3">${c2}</th></tr>`); }
    rows.push(`<tr><td><code>${name}</code></td><td>${p.name.replace(/^(A|B|AB) · /, '')}${/^AB/.test(p.name) ? (isB(p) ? ' (its B part)' : ' (its A part)') : ''}${isB(p) ? ' · engine B' : ''}</td><td>${p.stars ? '★ ' + p.stars : ''}</td></tr>`);
  }
  const n = h.replace(/<!-- PRESETS -->[\s\S]*<!-- \/PRESETS -->/, '<!-- PRESETS -->\n' + rows.join('\n') + '\n<!-- /PRESETS -->');
  if (n !== h) fs.writeFileSync(man, n);
}
// the web editor's mock device (web/editor.html, between /*WAVELET-BEGIN*/ and /*WAVELET-END*/): the engine's descriptors and presets
const ed = path.join(__dirname, '..', 'web', 'editor.html');
if (fs.existsSync(ed)) {
  const wl = ['HAAR', 'DB2', 'DB4', 'DB8', 'SYM8', 'COIF3', 'DB20'];
  const flt = []; for (const t of ['LP', 'HP', 'BP']) for (const s of [12, 18, 24, 36, 48, 96]) flt.push(t + s);
  const rowsM = [];
  for (const p of ordered) {
    const name = shortName(p.name);
    if (!name) continue;
    const v = Object.assign({}, DEF, p.v);
    // (the same mapping as above: recomputed from the generated header's line to stay in step)
    rowsM.push(name);
  }
  const hdr = fs.readFileSync(path.join(__dirname, '..', 'firmware', 'src', 'wavelet_presets.h'), 'utf8');
  const lines = hdr.split('\n').filter(l => /^    \{"/.test(l)).map(l => {
    const m = /^    \{"([^"]+)", \{([^}]*)\}, \{([^}]*)\}, (-?\d+), (\d+), FX/.exec(l);
    return `P("${m[1]}", [${m[2]}], [${m[3]}], ${m[5]}, 0)`;
  });
  const block = `    /*WAVELET-BEGIN*/
    { name: "WAVELET", titles: ["ATOMS", "SCALE"], edit: [E("WAVE", ${JSON.stringify(['SAW', 'SQR', 'PLS', 'TRI', 'SIN', 'NOIS', 'RAMP', 'MIRR', 'ALT', 'WALS', 'IMPL', 'RAND'])}, 0), E("WVLT", ${JSON.stringify(wl)}, 2),
        D("PHAS", F.PCT, 0, 127, 0), D("SPRS", F.PCT, 0, 127, 0), D("CUT", F.INT, 0, 127, 127), D("RES", F.PCT, 0, 127, 0),
        E("FLT", ${JSON.stringify(flt)}, 2), D("WNDW", F.PCT, 0, 127, 0)],
      presets: [${lines.join(',\n        ')}] },
    /*WAVELET-END*/`;
  const h = fs.readFileSync(ed, 'utf8');
  const n2 = h.replace(/ *\/\*WAVELET-BEGIN\*\/[\s\S]*?\/\*WAVELET-END\*\//, block);
  if (n2 !== h) fs.writeFileSync(ed, n2);
}
console.log(`wavelet_presets.h: ${kept} presets of ${src.presets.length} (${ordered.filter(p => p.top !== undefined && shortName(p.name)).length} TOP Sounds first)`);
