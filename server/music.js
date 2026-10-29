'use strict';
// Video export of the music-reactive 3D spirograph: the track is decoded and analysed here (same bands and the same Reactive logic the 3D page uses),
// each frame is drawn with a software 3D projection (a glowing line standing in for the tube), and ffmpeg muxes the picture with the original audio.
const path = require('path');
const { spawn } = require('child_process');
const os = require('os');
const { Worker, isMainThread, parentPort, workerData } = require('worker_threads');
const { createCanvas } = require('@napi-rs/canvas');

const SR = 44100, N = 2048;

// whole track as mono float samples
function decode(file) {
  return new Promise((resolve, reject) => {
    const ff = spawn('ffmpeg', ['-hide_banner', '-loglevel', 'error', '-protocol_whitelist', 'file', '-i', file, '-f', 'f32le', '-ac', '1', '-ar', String(SR), '-t', '360', 'pipe:1'], { stdio: ['ignore', 'pipe', 'pipe'] });
    const parts = []; let err = '';
    ff.stdout.on('data', (d) => parts.push(d)); ff.stderr.on('data', (d) => { err += d; });
    ff.on('error', reject);
    ff.on('close', (code) => {
      if (code) return reject(new Error('could not read the audio: ' + err.slice(-200)));
      const b = Buffer.concat(parts);
      resolve(new Float32Array(b.buffer.slice(b.byteOffset, b.byteOffset + b.length - (b.length % 4))));
    });
  });
}

function fft(re, im) {
  const n = re.length;
  for (let i = 1, j = 0; i < n; i++) {
    let bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) { [re[i], re[j]] = [re[j], re[i]]; [im[i], im[j]] = [im[j], im[i]]; }
  }
  for (let len = 2; len <= n; len <<= 1) {
    const a = -2 * Math.PI / len, wr = Math.cos(a), wi = Math.sin(a);
    for (let i = 0; i < n; i += len) {
      let cr = 1, ci = 0;
      for (let k = 0; k < len / 2; k++) {
        const u = i + k, v = u + len / 2, xr = re[v] * cr - im[v] * ci, xi = re[v] * ci + im[v] * cr;
        re[v] = re[u] - xr; im[v] = im[u] - xi; re[u] += xr; im[u] += xi;
        const t = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = t;
      }
    }
  }
}

// What the browser's AnalyserNode would report: Blackman window, magnitude/N, smoothing over time, then dB mapped to 0..255 (-100 to -30 dB).
function analyser(samples, fps) {
  const win = new Float64Array(N), tau = Math.pow(0.6, 60 / fps), prev = new Float64Array(N / 2), re = new Float64Array(N), im = new Float64Array(N), bytes = new Uint8Array(N / 2);
  for (let i = 0; i < N; i++) win[i] = 0.42 - 0.5 * Math.cos(2 * Math.PI * i / N) + 0.08 * Math.cos(4 * Math.PI * i / N);
  return (t) => {
    const end = Math.floor(t * SR);
    for (let i = 0; i < N; i++) { const k = end - N + i; re[i] = (k >= 0 && k < samples.length ? samples[k] : 0) * win[i]; im[i] = 0; }
    fft(re, im);
    for (let k = 0; k < N / 2; k++) {
      prev[k] = tau * prev[k] + (1 - tau) * Math.hypot(re[k], im[k]) / N;
      bytes[k] = Math.max(0, Math.min(255, Math.round(255 * (20 * Math.log10(prev[k] + 1e-12) + 100) / 70)));
    }
    const bw = SR / N, band = (lo, hi) => { let s = 0, c = 0; for (let b = Math.max(1, Math.round(lo / bw)); b <= Math.min(N / 2 - 1, Math.round(hi / bw)); b++) { s += bytes[b]; c++; } return c ? s / c / 255 : 0; };
    return [band(20, 150), band(150, 2000), band(2000, 8000)];
  };
}

/* ---- the curve: the same nested turns as the 3D page, in plain numbers ---- */
function frames(arms) {
  return arms.map((a) => {
    const t = a.tilt * Math.PI / 180, z = a.az * Math.PI / 180, ax = [Math.sin(t) * Math.cos(z), Math.cos(t), Math.sin(t) * Math.sin(z)], l = Math.hypot(...ax) || 1;
    const u = ax.map((v) => v / l), ref = Math.abs(u[0]) < 0.9 ? [1, 0, 0] : [0, 0, 1];
    let d = [u[1] * ref[2] - u[2] * ref[1], u[2] * ref[0] - u[0] * ref[2], u[0] * ref[1] - u[1] * ref[0]];
    const dl = Math.hypot(...d) || 1; d = d.map((v) => v / dl);
    const p = a.ph * Math.PI / 180, c = Math.cos(p), s = Math.sin(p), dot = u[0] * d[0] + u[1] * d[1] + u[2] * d[2], cr = [u[1] * d[2] - u[2] * d[1], u[2] * d[0] - u[0] * d[2], u[0] * d[1] - u[1] * d[0]];
    d = [0, 1, 2].map((i) => (d[i] * c + cr[i] * s + u[i] * dot * (1 - c)) * a.L);
    return { ax: u, d, w: Math.round(a.w) };
  });
}
function curveInto(buf, n, fr, c, kk) {
  const K = fr.length;
  for (let i = 0; i < n; i++) {
    const t = 2 * Math.PI * i / n;
    let x = 0, y = 0, z = 0;
    for (let k = K - 1; k >= 0; k--) {
      const f = fr[k], a = f.ax, ang = f.w * t, co = Math.cos(ang), si = Math.sin(ang);
      x += f.d[0]; y += f.d[1]; z += f.d[2];
      const dot = (a[0] * x + a[1] * y + a[2] * z) * (1 - co), cx = a[1] * z - a[2] * y, cy = a[2] * x - a[0] * z, cz = a[0] * y - a[1] * x;
      x = x * co + cx * si + a[0] * dot; y = y * co + cy * si + a[1] * dot; z = z * co + cz * si + a[2] * dot;
    }
    buf[3 * i] = (x - c[0]) * kk; buf[3 * i + 1] = (y - c[1]) * kk; buf[3 * i + 2] = (z - c[2]) * kk;
  }
  buf[3 * n] = buf[0]; buf[3 * n + 1] = buf[1]; buf[3 * n + 2] = buf[2];
}
function hsl(h, s, l) {                                      // h in 0..1
  const a = s * Math.min(l, 1 - l), f = (n) => { const k = (n + h * 12) % 12; return l - a * Math.max(-1, Math.min(k - 3, 9 - k, 1)); };
  return [f(0), f(8), f(4)].map((v) => Math.round(v * 255));
}

// Draws one frame from its parameters: { arms, hue, yaw, kick, lev0 }. Pure, so frames can be drawn on any thread in any order.
function drawer(spec, W, H) {
  const n = Math.min(H > 800 ? 6000 : 3500, Math.max(1000, Math.round(spec.pts || 6000))), buf = new Float32Array(3 * (n + 1));
  curveInto(buf, n, frames(spec.arms), [0, 0, 0], 1);                  // the fixed fit: centre and scale from the arms as set
  const lo = [Infinity, Infinity, Infinity], hi = [-Infinity, -Infinity, -Infinity];
  for (let i = 0; i < n; i++) for (let a = 0; a < 3; a++) { const v = buf[3 * i + a]; if (v < lo[a]) lo[a] = v; if (v > hi[a]) hi[a] = v; }
  const ctr = [0, 1, 2].map((a) => (lo[a] + hi[a]) / 2), ext = Math.max(hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]) || 1;
  const kk = (115 / ext) / (1 + 0.5 * spec.A.motion);
  const cv = createCanvas(W, H), g = cv.getContext('2d'), pitch = Math.atan2(60, 180), cp = Math.cos(pitch), sp = Math.sin(pitch), D = Math.hypot(60, 180), focal = (H / 2) / Math.tan(20 * Math.PI / 180);
  const X = new Float32Array(n + 1), Y = new Float32Array(n + 1), Z = new Float32Array(n + 1), core = Math.max(1.5, H / 230 * 0.9 * 1.2), step = Math.ceil(n / 450);
  g.lineCap = 'butt'; g.lineJoin = 'round';
  // Tubes: a lit sphere stamped at every curve sample, drawn far to near, so a run of them reads as a shaded tube with correct overlaps.
  const NB = 144, sprites = [], tube = spec.look !== 'line', order = new Uint32Array(n);
  if (tube) for (let b = 0; b < NB; b++) {
    const c = createCanvas(64, 64), q = c.getContext('2d'), h = b / NB, col = (l, s) => 'rgb(' + hsl(h, s, l).join(',') + ')', gr = q.createRadialGradient(22, 20, 2, 32, 32, 31);
    gr.addColorStop(0, col(0.92, 0.5)); gr.addColorStop(0.18, col(0.7, 0.8)); gr.addColorStop(0.6, col(0.45, 0.8)); gr.addColorStop(1, col(0.15, 0.75));
    q.fillStyle = gr; q.beginPath(); q.arc(32, 32, 31, 0, 2 * Math.PI); q.fill(); sprites.push(c);
  }
  const rad = (spec.tubeR || 1.3) / 100 * 115 / 1.15;           // tube radius in scene units (the curve is fitted to about 115 across)
  return (p) => {
    const zoom = kk * (1 + 0.15 * p.kick + 0.08 * p.lev0), cy = Math.cos(p.yaw), sy = Math.sin(p.yaw);   // the whole curve breathes with the bass and punches on beats
    curveInto(buf, n, frames(p.arms), ctr, zoom);
    for (let i = 0; i <= n; i++) {
      const px = buf[3 * i], py = buf[3 * i + 1], pz = buf[3 * i + 2], x = px * cy + pz * sy, z0 = -px * sy + pz * cy, y = py * cp - z0 * sp, z = py * sp + z0 * cp, s = focal / (D - z);
      X[i] = W / 2 + x * s; Y[i] = H / 2 - y * s; Z[i] = z;
    }
    g.fillStyle = '#282923'; g.fillRect(0, 0, W, H);
    if (tube) {
      for (let i = 0; i < n; i++) order[i] = i;
      order.sort((a, b) => Z[a] - Z[b]);
      const rr = rad * (1 + 0.5 * p.kick);
      for (let k = 0; k < n; k++) {
        const i = order[k], r = rr * focal / (D - Z[i]), b = Math.floor((((p.hue + spec.hrange * i / n) % 360) + 360) % 360 / 360 * NB) % NB;
        g.drawImage(sprites[b], X[i] - r, Y[i] - r, 2 * r, 2 * r);
      }
      return g.getImageData(0, 0, W, H).data;
    }
    for (const pass of [0, 1]) {                                // a wide dim pass under a bright core, so the line reads as a lit tube
      g.lineWidth = (pass ? core : core * 2.6) * (1 + 0.7 * p.kick);
      for (let i = 0; i < n; i += step) {
        const e = Math.min(n, i + step), [r, gg, b] = hsl((((p.hue + spec.hrange * i / n) % 360) + 360) % 360 / 360, 0.75, Math.min(0.85, (pass ? 0.6 : 0.45) + 0.2 * p.kick)), depth = Math.min(1, Math.max(0.3, 0.45 + 0.55 * (Z[i] + 60) / 120));
        g.strokeStyle = 'rgba(' + r + ',' + gg + ',' + b + ',' + (pass ? depth : depth * (0.28 + 0.3 * p.kick)) + ')';
        g.beginPath(); g.moveTo(X[i], Y[i]);
        for (let j = i + 1; j <= e; j++) g.lineTo(X[j], Y[j]);
        g.stroke();
      }
    }
    return g.getImageData(0, 0, W, H).data;
  };
}

if (!isMainThread && workerData && workerData.music) {
  const draw = drawer(workerData.spec, workerData.W, workerData.H);
  parentPort.on('message', ({ f, p }) => { const px = Buffer.from(draw(p)); parentPort.postMessage({ f, px }, [px.buffer]); });
}

// Returns { count, at(f) -> Promise<RGBA>, seconds, close() } for the server's ffmpeg helper.
// The audio is analysed in order first (cheap); the drawing, which is the slow part, runs on one worker thread per spare core.
async function prepare(audioFile, spec, W, H, fps, reactivePath) {
  const samples = await decode(audioFile), seconds = samples.length / SR;
  if (seconds < 1) throw new Error('the audio is empty');
  const R = require(reactivePath).create({ attack: 0.05, release: 0.06, kickDecay: 0.08 }), analyse = analyser(samples, fps), count = Math.floor(seconds * fps), params = [];
  let yaw = 0.6;
  for (let f = 0; f < count; f++) {
    const t = f / fps, out = R.step(analyse(t), 1 / fps, t * 1000, spec.arms, spec.A);
    if (spec.spin) yaw += (1 / fps) * (2 * Math.PI / 60) * out.rotSpeed;
    params.push({ arms: out.arms, hue: spec.hue + out.hueShift, yaw, kick: out.kick, lev0: out.lev[0] });
  }
  const nw = Math.max(1, Math.min(8, os.cpus().length - 1)), pool = [], waiting = new Map(), ahead = new Map();
  for (let i = 0; i < nw; i++) {
    const w = new Worker(__filename, { workerData: { music: true, spec, W, H } });
    w.busy = 0;
    w.on('message', ({ f, px }) => { w.busy--; const r = waiting.get(f); waiting.delete(f); r.resolve(new Uint8Array(px.buffer, px.byteOffset, px.length)); });
    w.on('error', (e) => { for (const r of waiting.values()) r.reject(e); waiting.clear(); });
    pool.push(w);
  }
  const ask = (f) => {
    const w = pool.reduce((a, b) => (b.busy < a.busy ? b : a)); w.busy++;
    return new Promise((resolve, reject) => { waiting.set(f, { resolve, reject }); w.postMessage({ f, p: params[f] }); });
  };
  const at = (f) => {
    for (let k = f; k < Math.min(count, f + 2 * nw); k++) if (!ahead.has(k)) ahead.set(k, ask(k));
    const p = ahead.get(f); ahead.delete(f); return p;
  };
  return { count, at, seconds, close: () => Promise.all(pool.map((w) => w.terminate())) };
}

module.exports = { prepare };
