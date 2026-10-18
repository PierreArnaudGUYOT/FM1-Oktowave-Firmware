#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Pierre Arnaud GUYOT (OktoPierre)
// The WAVELET engine's table in floating point: the reduced algorithm of eng_wavelet.c (512 samples, 9 bands)
// written as the Wavelet Synthesizer's core.js writes it (double precision). tests/wavelet_test.c writes the
// fixed-point tables it built as text; this compares them:
//   node tools/wavelet_ref.js build/host            (every build/host/table_*.txt; exit 1 over the tolerance)
// The fixed-point build keeps Q28 coefficients and Q20 samples, so the layers should match within a few
// units of their Q14 scale (-70 dB); the sparsity's choice of atoms can differ on exact ties only.
'use strict';
const fs = require('fs');
const path = require('path');
const W = JSON.parse(fs.readFileSync(path.join(__dirname, 'wavelets.json'), 'utf8'));
const N = 512, NB = 9, NL = 10;
const FILTERS = W.order.map(n => {
  const h = W.h[n], L = h.length, g = new Array(L);
  for (let i = 0; i < L; i++) g[i] = ((i & 1) ? -1 : 1) * h[L - 1 - i];
  return { h, g, L };
});

function rng(seed) { let s = seed >>> 0 || 1; return () => { s = (Math.imul(s, 1664525) + 1013904223) >>> 0; return s / 4294967296; }; }

// the source as eng_wavelet.c wav_source makes it: the wave at i / N + e2 / 128 (PLS / TRI: the width from e2)
function source(wave, e2) {
  const x = new Float64Array(N);
  const sh = wave === 2 || wave === 3 ? 0 : e2 / 128, width = 0.02 + e2 * 0.96 / 127;
  for (let i = 0; i < N; i++) {
    let ph = i / N + sh; ph -= Math.floor(ph);
    switch (wave) {
      case 0: x[i] = 1 - 2 * ph; break;
      case 1: x[i] = ph < 0.5 ? 1 : -1; break;
      case 2: x[i] = ph < width ? 1 : -1; break;
      case 3: x[i] = ph < width ? -1 + 2 * ph / width : 1 - 2 * (ph - width) / (1 - width); break;
      case 4: x[i] = Math.sin(2 * Math.PI * ph); break;
      default: x[i] = 0;
    }
  }
  if (wave >= 5) {
    const r = rng(12345), y = new Float64Array(N);
    for (let i = 0; i < N; i++) y[i] = r() * 2 - 1;
    const s = (e2 * 4) % N;
    for (let i = 0; i < N; i++) x[i] = y[(i + s) % N];
  }
  let mean = 0; for (let i = 0; i < N; i++) mean += x[i]; mean /= N;
  let peak = 0; for (let i = 0; i < N; i++) { x[i] -= mean; peak = Math.max(peak, Math.abs(x[i])); }
  if (peak > 0) for (let i = 0; i < N; i++) x[i] /= peak;
  return x;
}
function dwt(x, f) {
  const c = new Float64Array(N);
  let a = Float64Array.from(x), M = N;
  while (M > 1) {
    const half = M >> 1, na = new Float64Array(half);
    for (let k = 0; k < half; k++) {
      let sa = 0, sd = 0;
      for (let n = 0; n < f.L; n++) { const v = a[(2 * k + n) % M]; sa += f.h[n] * v; sd += f.g[n] * v; }
      na[k] = sa; c[half + k] = sd;
    }
    a = na; M = half;
  }
  c[0] = 0;
  return c;
}
function idwtBand(c, f, b) {
  const lo = 1 << (b - 1), hi = 1 << b;
  let a = new Float64Array(1);
  for (let M = 2; M <= N; M <<= 1) {
    const half = M >> 1, x = new Float64Array(M);
    for (let k = 0; k < half; k++) {
      const ak = a[k], dk = half + k >= lo && half + k < hi ? c[half + k] : 0;
      if (ak === 0 && dk === 0) continue;
      for (let n = 0; n < f.L; n++) { const i = (2 * k + n) % M; x[i] += f.h[n] * ak + f.g[n] * dk; }
    }
    a = x;
  }
  return a;
}
function sparsify(c, s) {
  let mx = 0; for (let i = 1; i < N; i++) mx = Math.max(mx, Math.abs(c[i]));
  const idx = [];
  // (the VST counts atoms down to 1e-9 of the largest; the firmware's 20-bit atoms stop at 2^-18: as it does)
  for (let i = 1; i < N; i++) { if (Math.abs(c[i]) > mx / 262144) idx.push(i); else c[i] = 0; }
  const nz = idx.length;
  const K = Math.min(nz, s <= 0 ? nz : Math.max(1, Math.round(Math.pow(nz, 1 - s))));
  if (K < nz) {
    idx.sort((a, b) => (Math.abs(c[b]) - Math.abs(c[a])) || (a - b));
    for (let j = K; j < nz; j++) c[idx[j]] = 0;
  }
  return { K, nz };
}
// the table as the firmware holds it: rows of the 9 layers and DRY, Q14
function table(wave, wavelet, e2, sprs) {
  const f = FILTERS[wavelet];
  const c = dwt(source(wave, e2), f);
  const mags = [], cmag = Float64Array.from(c, Math.abs);
  for (let i = 1; i < N; i++) mags.push(cmag[i]);
  mags.sort((a, b) => b - a);
  const sp = sparsify(c, sprs / 127);
  // a tie at the K-th largest atom: the fixed-point ranking may pick other atoms of the same size (rounding)
  const tie = sp.K < sp.nz && Math.abs(mags[sp.K - 1] - mags[sp.K]) <= 1e-6 * mags[0];
  const t = new Float64Array(N * NL);
  for (let b = 1; b <= NB; b++) {
    const l = idwtBand(c, f, b);
    for (let i = 0; i < N; i++) { t[i * NL + b - 1] = l[i] * 16384; t[i * NL + NL - 1] += l[i] * 16384; }
  }
  return { t, K: sp.K, nz: sp.nz, tie, c, cmag };
}

function compare(file) {
  const lines = fs.readFileSync(file, 'utf8').trim().split('\n');
  const m = /# wave (\d+) wavelet (\d+) e2 (\d+) sprs (\d+)/.exec(lines[0]);
  const ref = table(+m[1], +m[2], +m[3], +m[4]);
  if (/^# atoms/.test(lines[1])) {                      // the atoms the firmware kept against the reference's
    const fw = new Set(lines[1].slice(8).trim().split(/\s+/).filter(Boolean).map(a => +a.split(':')[0]));
    const rf = new Set(); for (let i = 1; i < N; i++) if (ref.c[i] !== 0) rf.add(i);
    const onlyFw = [...fw].filter(i => !rf.has(i)), onlyRf = [...rf].filter(i => !fw.has(i));
    const mag = i => (ref.cmag[i] * 1048576).toFixed(0);
    if (onlyFw.length || onlyRf.length)
      console.log(`  atoms kept only by the firmware: ${onlyFw.map(i => i + ' (' + mag(i) + ')').join(' ')}; ` +
        `only by the reference: ${onlyRf.map(i => i + ' (' + mag(i) + ')').join(' ')}`);
    lines.splice(1, 1);
  }
  let maxd = 0, sum2 = 0, n = 0, rms = 0;
  for (let i = 0; i < N; i++) {
    const v = lines[i + 1].split(' ').map(Number);
    for (let j = 0; j < NL; j++) {
      const d = v[j] - ref.t[i * NL + j];
      maxd = Math.max(maxd, Math.abs(d)); sum2 += d * d; n++; rms += ref.t[i * NL + j] ** 2;
    }
  }
  const err = Math.sqrt(sum2 / n), sig = Math.sqrt(rms / n);
  const db = 20 * Math.log10(err / sig);
  console.log(`${path.basename(file)}: wave ${m[1]} wavelet ${W.order[+m[2]]} sprs ${m[4]} (K ${ref.K} of ${ref.nz}` +
    `${ref.tie ? ', a tie at K: not compared' : ''}): max |diff| ${maxd.toFixed(1)}, rms error ${db.toFixed(1)} dB re the table`);
  return ref.tie ? -999 : db;
}

const dir = process.argv[2] || 'build/host';
const files = fs.readdirSync(dir).filter(f => /^table_.*\.txt$/.test(f)).sort();
if (!files.length) { console.error('no build/host/table_*.txt: run tests/wavelet_test.c first'); process.exit(2); }
let worst = -1e9;
for (const f of files) worst = Math.max(worst, compare(path.join(dir, f)));
const LIMIT = -55;   // dB: well under what the 16-bit output can show; sparsity ties may cost a little
console.log(worst <= LIMIT ? `OK: every table within ${LIMIT} dB of the reference (worst ${worst.toFixed(1)} dB)`
  : `FAIL: a table is ${worst.toFixed(1)} dB from the reference (limit ${LIMIT})`);
process.exit(worst <= LIMIT ? 0 : 1);
