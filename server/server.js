// Homelab build of Spirograph. Serves the same page as the public site, plus an API that does the heavy work:
// tracing names and uploaded pictures into Fourier terms, rendering PNG, JPG, SVG, GIF, MP4, PDF and plotter-SVG exports,
// splitting a picture into colour layers, and keeping saved designs and background render jobs on disk.
// The page's engine (geometry, FFT, tracing, stroking) is the page's own code, run here by ./engine.js.
const http = require('http');
const fs = require('fs');
const os = require('os');
const path = require('path');
const crypto = require('crypto');
const zlib = require('zlib');
const { spawn } = require('child_process');
const E = require('./engine');
const Face = require('./face');
const nodemailer = require('nodemailer');
const { loadImage } = require('@napi-rs/canvas');

const SITE = process.env.SITE_DIR || path.join(__dirname, '..', 'site');
const PORT = +process.env.PORT || 8080;
const MAX_BODY = 30e6;
const DATA = process.env.DATA_DIR || path.join(__dirname, '..', 'data');          // saved designs, pictures, finished renders
const PUBLIC_URL = (process.env.PUBLIC_URL || '').replace(/\/$/, '');             // used in notification links
const SMTP = { host: process.env.SMTP_HOST || '', port: +process.env.SMTP_PORT || 587, user: process.env.SMTP_USER || '', pass: process.env.SMTP_PASS || '' };
const MAIL_FROM = process.env.MAIL_FROM || '', MAIL_TO = process.env.MAIL_TO || '', DAILY_AT = process.env.DAILY_AT || '';   // DAILY_AT: "07:00" in TZ
const NTFY_URL = process.env.NTFY_URL || '', NTFY_TOKEN = process.env.NTFY_TOKEN || '', HA_WEBHOOK_URL = process.env.HA_WEBHOOK_URL || '';
for (const d of ['pics', 'designs', 'renders']) fs.mkdirSync(path.join(DATA, d), { recursive: true });
const TAU = Math.PI * 2;

/* ---------------------------------------------------------------- small helpers */
class HttpError extends Error { constructor(code, msg) { super(msg); this.code = code; } }
const num = (v, lo, hi, d) => { v = +v; return Number.isFinite(v) ? Math.min(hi, Math.max(lo, v)) : d; };
// least-recently-used map
class Lru extends Map {
  constructor(max) { super(); this.max = max; }
  get(k) { const v = super.get(k); if (v !== undefined) { super.delete(k); super.set(k, v); } return v; }
  set(k, v) { super.delete(k); super.set(k, v); if (this.size > this.max) super.delete(this.keys().next().value); return this; }
}
// heavy jobs run one at a time, so a few exports can't take the host down
let chain = Promise.resolve();
const queue = (fn) => { const r = chain.then(fn); chain = r.catch(() => {}); return r; };

/* ---------------------------------------------------------------- terms on the wire */
// [u32 meta length][meta JSON, padded to 4][Int32 fr][F32 cr][F32 ci][F32 rx][F32 ry][U8 up, padded to 4][U8 col x3]
function packTerms(T) {
  const K = T.fr.length, M = T.M, col = !!T.col;
  let meta = JSON.stringify({ K, M, c0r: T.c0r, c0i: T.c0i, mx: T.mx, my: T.my, bw: T.bw, bh: T.bh, col });
  while ((meta.length + 4) % 4) meta += ' ';
  const mb = Buffer.from(meta), upLen = (M + 3) & ~3;
  const out = Buffer.alloc(4 + mb.length + 12 * K + 8 * M + upLen + (col ? 3 * M : 0));
  let o = 0;
  o = out.writeUInt32LE(mb.length, o); o += mb.copy(out, o);
  const put = (Arr, src, n) => { const a = new Arr(out.buffer, out.byteOffset + o, n); a.set(src); o += n * 4; };
  put(Int32Array, T.fr, K); put(Float32Array, T.cr, K); put(Float32Array, T.ci, K); put(Float32Array, T.rx, M); put(Float32Array, T.ry, M);
  out.set(T.up, o); o += upLen;
  if (col) out.set(T.col, o);
  return out;
}

/* ---------------------------------------------------------------- pictures */
const pics = new Lru(10);           // id -> { src canvas }: the decoded picture, kept so sliders and exports don't re-upload it
const picTermsCache = new Lru(16);  // id|thr|detail -> packed terms
const PIC_ID = /^[0-9a-f]{12}$/, picFile = (id) => path.join(DATA, 'pics', id);
// Every upload is also kept on disk, so saved designs and a restarted server still have their pictures.
async function decodePic(id, bytes) {
  let img;
  try { img = await loadImage(bytes); } catch (_) { throw new HttpError(415, 'That file could not be read as an image'); }
  const k = Math.min(1, 4096 / Math.max(img.width, img.height)), src = E.createCanvas(Math.max(1, Math.round(img.width * k)), Math.max(1, Math.round(img.height * k)));
  const c = src.getContext('2d');
  c.drawImage(img, 0, 0, src.width, src.height);
  // a cut-out picture (transparent background): its alpha is the subject mask
  const px = c.getImageData(0, 0, src.width, src.height).data, alpha = new Uint8Array(src.width * src.height);
  let clear = 0;
  for (let i = 0; i < alpha.length; i++) { alpha[i] = px[4 * i + 3] > 128 ? 1 : 0; if (!alpha[i]) clear++; }
  c.globalCompositeOperation = 'destination-over';
  c.fillStyle = '#fff'; c.fillRect(0, 0, src.width, src.height);                // transparent areas read as white
  pics.set(id, { alpha: clear > alpha.length * 0.02 ? alpha : null, src, lines: fs.existsSync(picFile(id) + '.lines') });   // .lines: a line drawing (face art), traced by its lines, not its edges
}
async function storePic(bytes, lines = false) {
  const id = crypto.createHash('sha1').update(bytes).digest('hex').slice(0, 12);
  if (lines) fs.writeFileSync(picFile(id) + '.lines', '');
  const was = pics.get(id);
  if (was && lines) was.lines = true;
  if (!pics.get(id)) await decodePic(id, bytes);
  if (!fs.existsSync(picFile(id))) fs.writeFileSync(picFile(id), bytes);
  return id;
}
// A face drawing is a vector path, not a picture: the points are kept in <id>.paths, with a blank stand-in image.
const pathFile = (id) => picFile(id) + '.paths';
function holdPaths(id, paths) { pics.set(id, { src: E.createCanvas(2, 2), paths }); }
// Make sure every picture the settings mention is decoded (from disk if need be) before the synchronous cleanP.
async function needPics(...ps) {
  for (const p of ps) {
    const id = p && p.pic && p.pic.id;
    if (typeof id !== 'string' || pics.get(id)) continue;
    if (PIC_ID.test(id) && fs.existsSync(pathFile(id))) { holdPaths(id, JSON.parse(fs.readFileSync(pathFile(id), 'utf8'))); continue; }
    if (!PIC_ID.test(id) || !fs.existsSync(picFile(id))) throw new HttpError(410, 'picture ' + id + ' is not on the server');
    await decodePic(id, fs.readFileSync(picFile(id)));
  }
}
const vectorTermsCache = new Lru(16);
function vectorTerms(id, paths) {
  let T = vectorTermsCache.get(id);
  if (!T) { T = E.pathTerms(paths.ox, paths.oy, 80); vectorTermsCache.set(id, T); }
  return T;
}
function picObject(m) {                // a client's picture description -> what the engine's picTerms expects
  const held = m && typeof m.id === 'string' && pics.get(m.id);
  if (!held) throw new HttpError(410, 'picture ' + (m && m.id) + ' is not on the server');
  const thr = num(m.thr, 5, 95, 30), detail = Math.round(num(m.detail, 96, 4096, 320));
  // a vector drawing comes with its terms already built (the thresholds and detail do not apply to it)
  return { src: held.src, name: String(m.name || '').slice(0, 60), thr, detail, match: !!m.match, clean: !!m.clean, lines: !!held.lines,
    key: held.paths ? thr + ',' + detail + (m.clean ? ',c' : '') : '', T: held.paths ? vectorTerms(m.id, held.paths) : null };
}
function picTerms(id, thr, detail, clean) {
  const key = id + '|' + thr + '|' + detail + (clean ? '|c' : '');
  let buf = picTermsCache.get(key);
  if (!buf) {
    const T = E.picTerms(picObject({ id, thr, detail, clean, match: true }));
    buf = T ? packTerms(T) : Buffer.alloc(0);
    picTermsCache.set(key, buf);
  }
  return buf;
}

/* ---------------------------------------------------------------- names */
const nameTermsCache = new Lru(32);
function nameTerms(lines) {
  const key = lines.join('\n');
  let buf = nameTermsCache.get(key);
  if (!buf) { const T = E.nameTerms(lines); buf = T ? packTerms(T) : Buffer.alloc(0); nameTermsCache.set(key, buf); }
  return buf;
}

/* ---------------------------------------------------------------- settings from the client */
const STR_KEYS = new Set(['ringShape', 'wheelShape', 'side', 'ink', 'sizing', 'w2side', 'text', 'color']);
// Copy only the page's own settings, with the right types; anything else is dropped.
function cleanP(raw) {
  const p = Object.assign({}, E.DEFAULTS);
  if (!raw || typeof raw !== 'object') return p;
  for (const k of Object.keys(E.DEFAULTS)) {
    const d = E.DEFAULTS[k], v = raw[k];
    if (v === undefined) continue;
    if (typeof d === 'boolean') p[k] = !!v;
    else if (typeof d === 'number') p[k] = num(v, -1e4, 1e5, d);
    else if (STR_KEYS.has(k) && typeof v === 'string') p[k] = v.slice(0, E.MAX_TEXT);
  }
  if (!E.SHAPES.includes(p.ringShape)) p.ringShape = E.DEFAULTS.ringShape;
  if (!E.SHAPES.includes(p.wheelShape)) p.wheelShape = E.DEFAULTS.wheelShape;
  if (p.side !== 'inside' && p.side !== 'outside') p.side = E.DEFAULTS.side;
  if (p.w2side !== 'inside' && p.w2side !== 'outside') p.w2side = E.DEFAULTS.w2side;
  if (p.sizing !== 'pct' && p.sizing !== 'teeth') p.sizing = E.DEFAULTS.sizing;
  if (!Object.prototype.hasOwnProperty.call(E.INKS, p.ink)) p.ink = E.DEFAULTS.ink;
  if (!/^#[0-9a-f]{6}$/i.test(p.color)) p.color = E.DEFAULTS.color;
  if (raw.pic && typeof raw.pic === 'object') p.pic = picObject(raw.pic);
  return p;
}
const build = (p, w, h) => E.buildModel(p, w, h);
// A geometric pattern that pokes past the square is rebuilt with a smaller ring so all of it shows (as in the page).
function buildInSquare(p, side) {
  const m = build(p, side, side);
  if (m.fourier) return m;
  E.fill(m, m.N);
  let ext = 0;
  for (let i = 0; i <= m.N; i++) ext = Math.max(ext, Math.abs(m.xs[i] - m.cx), Math.abs(m.ys[i] - m.cy));
  const half = side / 2 - 6;
  return ext > half ? build(Object.assign({}, p, { ring: p.ring * half / ext }), side, side) : m;
}

/* ---------------------------------------------------------------- stills */
function bounds(models) {
  let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity;
  for (const m of models) {
    E.fill(m, m.N);
    for (let i = 0; i <= m.N; i++) {
      const x = m.xs[i], y = m.ys[i];
      if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y;
    }
  }
  return x0 <= x1 ? { x0, y0, x1, y1 } : null;
}
// The screen rectangle, grown on any side the design pokes past, so nothing is clipped (same rule as the page).
function region(b, vw, vh) {
  const pad = 12;
  let x0 = 0, y0 = 0, x1 = vw, y1 = vh;
  if (b) {
    if (b.x0 < pad) x0 = b.x0 - pad;
    if (b.y0 < pad) y0 = b.y0 - pad;
    if (b.x1 > vw - pad) x1 = b.x1 + pad;
    if (b.y1 > vh - pad) y1 = b.y1 + pad;
  }
  return { x0, y0, w: x1 - x0, h: y1 - y0 };
}
function models(body) {
  const vw = Math.round(num(body.vw, 200, 4000, 1280)), vh = Math.round(num(body.vh, 200, 4000, 800));
  const layers = (Array.isArray(body.layers) ? body.layers : []).slice(0, 24).map((l) => build(cleanP(l), vw, vh));
  return { vw, vh, layers, cur: build(cleanP(body.P), vw, vh) };
}
function renderStill(body, fmt, maxPx = 8192) {
  const { vw, vh, layers, cur } = models(body);
  const all = layers.concat([cur]), R = region(bounds(all), vw, vh);
  // up to 8x the screen, 8192 px on the long side, 36 megapixels
  const scale = Math.min(8, maxPx / Math.max(R.w, R.h), Math.sqrt(36e6 / (R.w * R.h)));
  const cv = E.createCanvas(Math.round(R.w * scale), Math.round(R.h * scale)), ctx = cv.getContext('2d');
  const bg = fmt === 'jpg' ? (body.bg || '#282923') : body.bg;                  // JPEG has no alpha
  if (bg && /^#[0-9a-f]{6}$/i.test(bg)) { ctx.fillStyle = bg; ctx.fillRect(0, 0, cv.width, cv.height); }
  E.prepCtx(ctx, scale);
  ctx.translate(-R.x0, -R.y0);
  for (const m of all) E.strokeRange(ctx, m, 0, m.N);
  return fmt === 'jpg' ? { type: 'image/jpeg', buf: cv.toBuffer('image/jpeg', 95) } : { type: 'image/png', buf: cv.toBuffer('image/png') };
}
// Vector export: points closer than 0.25 px to the last are dropped; colour-cycled or picture-matched
// patterns are split into coloured paths.
function chunks(m) {
  const match = E.matching(m), step = match ? Math.ceil(m.N / 400) : m.p.ink === 'cycle' ? Math.ceil(m.N / 90) : m.N, out = [];
  for (let i = 0; i < m.N; i += step) { const e = Math.min(m.N, i + step); out.push([i, e, match ? E.picColor(m, i, e) : E.inkRGB(m.p, i / m.N)]); }
  return out;
}
function renderSVG(body) {
  const { vw, vh, layers, cur } = models(body), all = layers.concat([cur]), R = region(bounds(all), vw, vh), out = [];
  for (const m of all) {
    for (const [i, e, c] of chunks(m)) {
      let lx = m.xs[i], ly = m.ys[i], d = 'M' + lx.toFixed(1) + ' ' + ly.toFixed(1);
      for (let j = i + 1; j <= e; j++) {
        const x = m.xs[j], y = m.ys[j], pen = m.up && m.up[j];
        if (!pen && j < e && (x - lx) * (x - lx) + (y - ly) * (y - ly) < 0.0625) continue;
        d += (pen ? 'M' : 'L') + x.toFixed(1) + ' ' + y.toFixed(1);
        lx = x; ly = y;
      }
      out.push('<path d="' + d + '" stroke="rgb(' + c.join(',') + ')" stroke-width="' + m.p.weight + '"/>');
    }
  }
  const bg = body.bg && /^#[0-9a-f]{6}$/i.test(body.bg) ? '<rect x="' + R.x0 + '" y="' + R.y0 + '" width="' + R.w + '" height="' + R.h + '" fill="' + body.bg + '"/>' : '';
  const svg = '<svg xmlns="http://www.w3.org/2000/svg" width="' + R.w + '" height="' + R.h + '" viewBox="' + R.x0 + ' ' + R.y0 + ' ' + R.w + ' ' + R.h + '">' + bg +
    '<g fill="none" stroke-opacity="' + E.INK_ALPHA + '" stroke-linejoin="round">' + out.join('') + '</g></svg>';
  return { type: 'image/svg+xml', buf: Buffer.from(svg) };
}

/* ---------------------------------------------------------------- animation (GIF, MP4) */
// Square frames: kept layers are drawn once, the live pattern grows stroke by stroke, and the circles or wheels sit on top.
function frameSource(body, side) {
  const vw = Math.round(num(body.vw, 200, 4000, 1280)), vh = Math.round(num(body.vh, 200, 4000, 800)), z = Math.min(vw, vh) / side;
  const P = cleanP(body.P), m = buildInSquare(P, side), n = m.N, wheels = P.showWheels;
  const mk = () => E.createCanvas(side, side);
  const out = mk(), octx = out.getContext('2d'), tr = mk(), trc = tr.getContext('2d'), bs = mk(), bsc = bs.getContext('2d');
  for (const c of [trc, bsc]) { E.prepCtx(c, 1); c.z = z; }
  for (const l of (Array.isArray(body.layers) ? body.layers : []).slice(0, 24)) { const k = buildInSquare(cleanP(l), side); E.strokeRange(bsc, k, 0, k.N); }
  let at = 0;
  return {
    n,
    frame(i) {
      if (i > at) { E.strokeRange(trc, m, at, i); at = i; }
      octx.fillStyle = '#282923'; octx.fillRect(0, 0, side, side);
      octx.drawImage(bs, 0, 0); octx.drawImage(tr, 0, 0);
      if (wheels || (m.fourier && i < n)) E.drawWheels(octx, i, z, m);
      return octx.getImageData(0, 0, side, side).data;
    }
  };
}
function ffmpeg(args, frames, ext, o = {}) {
  const file = path.join(os.tmpdir(), 'spiro-' + crypto.randomBytes(6).toString('hex') + '.' + ext);
  return new Promise((resolve, reject) => {
    const av = ['-hide_banner', '-loglevel', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'rgba', ...args, file];
    // background jobs run at low priority so tracing and quick exports stay responsive
    const ff = o.long ? spawn('nice', ['-n', '10', 'ffmpeg', ...av], { stdio: ['pipe', 'ignore', 'pipe'] }) : spawn('ffmpeg', av, { stdio: ['pipe', 'ignore', 'pipe'] });
    let err = '';
    const kill = setTimeout(() => ff.kill('SIGKILL'), o.long ? 3.6e6 : 240e3);
    ff.stderr.on('data', (d) => { err += d; });
    ff.on('error', (e) => { clearTimeout(kill); reject(e); });
    ff.on('close', (code) => {
      clearTimeout(kill);
      if (code || (o.job && o.job.cancel)) { fs.rm(file, () => {}); return reject(new Error('ffmpeg exit ' + code + ': ' + err.slice(-300))); }
      const buf = fs.readFileSync(file); fs.rm(file, () => {});
      resolve(buf);
    });
    ff.stdin.on('error', () => {});
    (async () => {
      for (let f = 0; f < frames.count; f++) {
        if (o.job) { o.job.done = f; o.job.total = frames.count; if (o.job.cancel) { ff.kill('SIGKILL'); return; } }
        const fr = frames.at(f), px = Buffer.from(fr.buffer, fr.byteOffset, fr.length);
        if (!ff.stdin.write(px)) await new Promise((r) => ff.stdin.once('drain', r));
        await new Promise((r) => setImmediate(r));                      // let other requests run between frames
      }
      ff.stdin.end();
    })().catch(reject);
  });
}
// o: { long } for a background job (up to 60 s, 4K, 60 fps), { size, fps } its choices.
async function renderAnimation(body, fmt, o = {}) {
  const gif = fmt === 'gif', side = gif ? 480 : o.long ? Math.round(num(o.size, 480, 2160, 1080)) & ~1 : 1080, fps = gif ? 25 : o.long ? Math.round(num(o.fps, 24, 60, 30)) : 30;
  const seconds = gif ? 6 : Math.round(num(o.long ? o.seconds : body.seconds, 3, o.long ? 60 : 20, 9)), hold = Math.round(fps * 1.5);
  const src = frameSource(body, side), draw = fps * seconds, total = draw + hold;
  const frames = { count: total, at: (f) => src.frame(Math.round(src.n * Math.min(1, (f + 1) / draw))) };
  const size = side + 'x' + side;
  if (gif) {
    const buf = await ffmpeg(['-s', size, '-r', String(fps), '-i', '-', '-vf', 'split[a][b];[a]palettegen=stats_mode=diff[p];[b][p]paletteuse=dither=bayer:bayer_scale=5:diff_mode=rectangle', '-loop', '0'], frames, 'gif', o);
    return { type: 'image/gif', buf };
  }
  const buf = await ffmpeg(['-s', size, '-r', String(fps), '-i', '-', '-c:v', 'libx264', '-preset', 'medium', '-crf', '17', '-pix_fmt', 'yuv420p', '-movflags', '+faststart'], frames, 'mp4', o);
  return { type: 'video/mp4', buf };
}

/* ---------------------------------------------------------------- print: PDF and plotter SVG */
// Vector output on a real page, in millimetres. The design is scaled to fit inside the margin and centred.
const PAGES = { a4: [210, 297], a3: [297, 420], a2: [420, 594], letter: [215.9, 279.4] };
function layout(body, o) {
  const { vw, vh, layers, cur } = models(body), all = layers.concat([cur]), R = region(bounds(all), vw, vh);
  const [a, b] = PAGES[o.page] || PAGES.a4, land = R.w > R.h, pw = land ? b : a, ph = land ? a : b, mg = num(o.margin, 0, 50, 10);
  const s = Math.min((pw - 2 * mg) / R.w, (ph - 2 * mg) / R.h), ox = (pw - R.w * s) / 2 - R.x0 * s, oy = (ph - R.h * s) / 2 - R.y0 * s;
  return { all, pw, ph, s, tx: (x) => x * s + ox, ty: (y) => y * s + oy };
}
// Samples i..e as moves and lines; points closer than minStep to the last are dropped.
function pathData(m, i, e, tx, ty, minStep, emit) {
  let lx = tx(m.xs[i]), ly = ty(m.ys[i]);
  emit(true, lx, ly);
  for (let j = i + 1; j <= e; j++) {
    const x = tx(m.xs[j]), y = ty(m.ys[j]), pen = m.up && m.up[j];
    if (!pen && j < e && (x - lx) * (x - lx) + (y - ly) * (y - ly) < minStep * minStep) continue;
    emit(!!pen, x, y);
    lx = x; ly = y;
  }
}
const hexRGB = (h) => [1, 3, 5].map((i) => parseInt(h.slice(i, i + 2), 16));
// Plain RGB -> CMYK (no colour profile): what a print shop needs to see in the file, not a colour-managed conversion.
function cmykOf([r, g, b]) {
  r /= 255; g /= 255; b /= 255;
  const k = 1 - Math.max(r, g, b);
  return k >= 1 ? [0, 0, 0, 1] : [(1 - r - k) / (1 - k), (1 - g - k) / (1 - k), (1 - b - k) / (1 - k), k];
}
function pdfFile(wPt, hPt, content, alpha) {
  const o = ['<< /Type /Catalog /Pages 2 0 R >>', '<< /Type /Pages /Kids [3 0 R] /Count 1 >>',
    '<< /Type /Page /Parent 2 0 R /MediaBox [0 0 ' + wPt.toFixed(2) + ' ' + hPt.toFixed(2) + '] /Contents 4 0 R /Resources << /ExtGState << /G0 << /CA ' + alpha + ' /ca ' + alpha + ' >> >> >> >>',
    '<< /Length ' + Buffer.byteLength(content) + ' >>\nstream\n' + content + '\nendstream'];
  let s = '%PDF-1.4\n'; const xs = [];
  o.forEach((b, i) => { xs.push(s.length); s += (i + 1) + ' 0 obj\n' + b + '\nendobj\n'; });          // ASCII only: length = bytes
  const x0 = s.length;
  s += 'xref\n0 ' + (o.length + 1) + '\n0000000000 65535 f \n' + xs.map((n) => String(n).padStart(10, '0') + ' 00000 n \n').join('') +
    'trailer\n<< /Size ' + (o.length + 1) + ' /Root 1 0 R >>\nstartxref\n' + x0 + '\n%%EOF\n';
  return Buffer.from(s, 'latin1');
}
// Reorders pen strokes to cut the pen's travel: from the pen's position always take the nearest stroke end, drawing that stroke backwards
// when the far end is the near one. Greedy, O(n^2): past 4000 strokes the order is left as drawn.
function orderStrokes(strokes) {
  const gap = (ss) => { let t = 0; for (let i = 1; i < ss.length; i++) { const a = ss[i - 1][ss[i - 1].length - 1], b = ss[i][0]; t += Math.hypot(a[0] - b[0], a[1] - b[1]); } return t; };
  const before = gap(strokes);
  if (strokes.length < 3 || strokes.length > 4000) return { strokes, before, after: before };
  const left = strokes.slice(), out = [left.shift()];
  while (left.length) {
    const e = out[out.length - 1][out[out.length - 1].length - 1];
    let bi = 0, bd = Infinity, flip = false;
    for (let i = 0; i < left.length; i++) {
      const st = left[i], d0 = Math.hypot(st[0][0] - e[0], st[0][1] - e[1]), d1 = Math.hypot(st[st.length - 1][0] - e[0], st[st.length - 1][1] - e[1]);
      if (d0 < bd) { bd = d0; bi = i; flip = false; }
      if (d1 < bd) { bd = d1; bi = i; flip = true; }
    }
    const st = left.splice(bi, 1)[0];
    out.push(flip ? st.reverse() : st);
  }
  return { strokes: out, before, after: gap(out) };
}
function renderPrint(body, fmt, o = {}) {
  const { all, pw, ph, s, tx, ty } = layout(body, o);
  if (fmt === 'plotter') {                       // one path per layer (one pen per layer), thin black lines, no background
    const pen = num(o.pen, 0.05, 5, 0.3);
    let before = 0, after = 0;
    const out = all.map((m, n) => {
      const strokes = [];                          // a stroke is a run of points drawn with the pen down; pen lifts split them
      pathData(m, 0, m.N, tx, ty, 0.05, (mv, x, y) => { if (mv) strokes.push([]); strokes[strokes.length - 1].push([x, y]); });
      const ord = orderStrokes(strokes);
      before += ord.before; after += ord.after;
      let d = '';
      for (const st of ord.strokes) st.forEach(([x, y], j) => { d += (j ? 'L' : 'M') + x.toFixed(2) + ' ' + y.toFixed(2); });
      return '<path id="layer-' + (n + 1) + '" d="' + d + '" stroke="rgb(' + (o.colors ? E.inkRGB(m.p, 0.5) : [0, 0, 0]).join(',') + ')"/>';
    });
    const svg = '<svg xmlns="http://www.w3.org/2000/svg" width="' + pw + 'mm" height="' + ph + 'mm" viewBox="0 0 ' + pw + ' ' + ph + '">' +
      '<!-- pen-up travel ' + Math.round(before) + ' mm in drawing order, ' + Math.round(after) + ' mm ordered -->' +
      '<g fill="none" stroke-width="' + pen + '" stroke-linecap="round" stroke-linejoin="round">' + out.join('') + '</g></svg>';
    return { type: 'image/svg+xml', buf: Buffer.from(svg), ext: 'svg', travel: { before: Math.round(before), after: Math.round(after) } };
  }
  const MM = 72 / 25.4, cm = !!o.cmyk, col = (c, stroke) => cm ? cmykOf(c).map((v) => v.toFixed(3)).join(' ') + (stroke ? ' K' : ' k') : c.map((v) => (v / 255).toFixed(3)).join(' ') + (stroke ? ' RG' : ' rg');
  let c = '1 J 1 j\n/G0 gs\n';
  if (body.bg && /^#[0-9a-f]{6}$/i.test(body.bg)) c += col(hexRGB(body.bg), false) + '\n0 0 ' + (pw * MM).toFixed(2) + ' ' + (ph * MM).toFixed(2) + ' re f\n';
  for (const m of all) {
    for (const [i, e, rgb] of chunks(m)) {
      c += col(rgb, true) + '\n' + (m.p.weight * s * MM).toFixed(3) + ' w\n';
      pathData(m, i, e, tx, ty, 0.05, (mv, x, y) => { c += (x * MM).toFixed(2) + ' ' + ((ph - y) * MM).toFixed(2) + (mv ? ' m\n' : ' l\n'); });
      c += 'S\n';
    }
  }
  return { type: 'application/pdf', buf: pdfFile(pw * MM, ph * MM, c, E.INK_ALPHA), ext: 'pdf' };
}

/* ---------------------------------------------------------------- any format, and zip */
function renderAny(b, fmt, o = {}) {
  if (fmt === 'gif' || fmt === 'mp4') return renderAnimation(b, fmt, o);
  if (fmt === 'svg') return renderSVG(b);
  if (fmt === 'pdf' || fmt === 'plotter') return renderPrint(b, fmt, o);
  return renderStill(b, fmt, o.maxPx);
}
const EXT = { png: 'png', jpg: 'jpg', svg: 'svg', gif: 'gif', mp4: 'mp4', pdf: 'pdf', plotter: 'svg' };
// A zip with the files stored, not compressed (PNG, JPEG and PDF gain little from it).
function zipFiles(files) {
  const parts = [], dir = [];
  let off = 0;
  for (const f of files) {
    const nm = Buffer.from(f.name), crc = zlib.crc32(f.buf), h = Buffer.alloc(30), d = Buffer.alloc(46);
    h.writeUInt32LE(0x04034b50, 0); h.writeUInt16LE(20, 4); h.writeUInt16LE(0x0800, 6); h.writeUInt16LE(0x21, 12);
    h.writeUInt32LE(crc, 14); h.writeUInt32LE(f.buf.length, 18); h.writeUInt32LE(f.buf.length, 22); h.writeUInt16LE(nm.length, 26);
    d.writeUInt32LE(0x02014b50, 0); d.writeUInt16LE(20, 4); d.writeUInt16LE(20, 6); d.writeUInt16LE(0x0800, 8); d.writeUInt16LE(0x21, 14);
    d.writeUInt32LE(crc, 16); d.writeUInt32LE(f.buf.length, 20); d.writeUInt32LE(f.buf.length, 24); d.writeUInt16LE(nm.length, 28); d.writeUInt32LE(off, 42);
    parts.push(h, nm, f.buf); dir.push(d, nm);
    off += 30 + nm.length + f.buf.length;
  }
  const cd = Buffer.concat(dir), end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0); end.writeUInt16LE(files.length, 8); end.writeUInt16LE(files.length, 10); end.writeUInt32LE(cd.length, 12); end.writeUInt32LE(off, 16);
  return Buffer.concat([...parts, cd, end]);
}
const safeName = (t) => String(t).replace(/[\\/:*?"<>|\x00-\x1f]+/g, ' ').replace(/\s+/g, ' ').trim().slice(0, 60) || 'Spirograph';

/* ---------------------------------------------------------------- picture -> colour layers */
// k-means on the picture's colours; each colour becomes a black-on-white mask picture of its own, traced as its own layer.
async function splitPic(id, k) {
  const held = pics.get(id);
  if (!held) throw new HttpError(410, 'picture ' + id + ' is not on the server');
  const { src } = held, W = src.width, H = src.height, px = src.getContext('2d').getImageData(0, 0, W, H).data;
  const step = Math.max(1, Math.floor(Math.sqrt(W * H / 20000))), S = [];
  for (let y = 0; y < H; y += step) for (let x = 0; x < W; x += step) { const i = 4 * (y * W + x); S.push(px[i], px[i + 1], px[i + 2]); }
  const n = S.length / 3, C = [];
  let mr = 0, mg = 0, mb = 0;
  for (let i = 0; i < n; i++) { mr += S[3 * i]; mg += S[3 * i + 1]; mb += S[3 * i + 2]; }
  C.push([mr / n, mg / n, mb / n]);
  const dist = (i, c) => (S[3 * i] - c[0]) ** 2 + (S[3 * i + 1] - c[1]) ** 2 + (S[3 * i + 2] - c[2]) ** 2;
  while (C.length < k) {                                     // farthest-point start: deterministic and spread out
    let best = 0, bd = -1;
    for (let i = 0; i < n; i++) { const d = Math.min(...C.map((c) => dist(i, c))); if (d > bd) { bd = d; best = i; } }
    C.push([S[3 * best], S[3 * best + 1], S[3 * best + 2]]);
  }
  for (let it = 0; it < 10; it++) {
    const sum = C.map(() => [0, 0, 0, 0]);
    for (let i = 0; i < n; i++) {
      let b = 0, bd = Infinity;
      for (let c = 0; c < k; c++) { const d = dist(i, C[c]); if (d < bd) { bd = d; b = c; } }
      const a = sum[b]; a[0] += S[3 * i]; a[1] += S[3 * i + 1]; a[2] += S[3 * i + 2]; a[3]++;
    }
    sum.forEach((a, c) => { if (a[3]) C[c] = [a[0] / a[3], a[1] / a[3], a[2] / a[3]]; });
  }
  const lab = new Uint8Array(W * H), count = new Array(k).fill(0), edge = new Array(k).fill(0);
  for (let p = 0; p < W * H; p++) {
    let b = 0, bd = Infinity;
    for (let c = 0; c < k; c++) { const d = (px[4 * p] - C[c][0]) ** 2 + (px[4 * p + 1] - C[c][1]) ** 2 + (px[4 * p + 2] - C[c][2]) ** 2; if (d < bd) { bd = d; b = c; } }
    lab[p] = b; count[b]++;
    const x = p % W, y = (p / W) | 0;
    if (x === 0 || y === 0 || x === W - 1 || y === H - 1) edge[b]++;
  }
  const border = 2 * (W + H) - 4, out = [];
  for (let c = 0; c < k; c++) {
    if (count[c] < W * H * 0.005 || edge[c] > 0.5 * border) continue;     // specks, and the background (it owns most of the border)
    const cv = E.createCanvas(W, H), cx = cv.getContext('2d'), img = cx.createImageData(W, H);
    for (let p = 0; p < W * H; p++) { const v = lab[p] === c ? 0 : 255; img.data[4 * p] = img.data[4 * p + 1] = img.data[4 * p + 2] = v; img.data[4 * p + 3] = 255; }
    cx.putImageData(img, 0, 0);
    out.push({ id: await storePic(cv.toBuffer('image/png')), color: '#' + C[c].map((v) => Math.round(v).toString(16).padStart(2, '0')).join(''), share: count[c] / (W * H) });
  }
  return out.sort((a, b) => b.share - a.share);
}

/* ---------------------------------------------------------------- subject cut-out (neural network) */
// U2-Net (small, Apache-2.0) finds the main subject of a picture; the picture is then redrawn with only that subject
// (or as its black silhouette), which the usual edge tracing turns into a clean outline instead of background clutter.
// the full U2-Net (176 MB) when it is there, else the small one (4.6 MB)
const MODEL = process.env.MODEL_PATH || ['u2net', 'u2netp'].map((n) => path.join(__dirname, 'models', n + '.onnx')).find((f) => fs.existsSync(f));
let seg = null;
async function subjectMask(src) {
  if (!seg) {
    try { const ort = require('onnxruntime-node'); seg = { ort, s: await ort.InferenceSession.create(MODEL, { intraOpNumThreads: 4 }) }; }
    catch (e) { console.error(e); throw new HttpError(501, 'the outline model is not installed on this server'); }
  }
  const S = 320, cv = E.createCanvas(S, S), cx = cv.getContext('2d');
  cx.drawImage(src, 0, 0, S, S);
  const px = cx.getImageData(0, 0, S, S).data, f = new Float32Array(3 * S * S), mean = [0.485, 0.456, 0.406], sd = [0.229, 0.224, 0.225];
  let mx = 1e-6;
  for (let i = 0; i < px.length; i++) if (i % 4 < 3 && px[i] > mx) mx = px[i];
  for (let p = 0; p < S * S; p++) for (let c = 0; c < 3; c++) f[c * S * S + p] = (px[4 * p + c] / mx - mean[c]) / sd[c];
  const out = await seg.s.run({ [seg.s.inputNames[0]]: new seg.ort.Tensor('float32', f, [1, 3, S, S]) }), d = out[seg.s.outputNames[0]].data;
  let lo = Infinity, hi = -Infinity;
  for (const v of d) { if (v < lo) lo = v; if (v > hi) hi = v; }
  const mc = E.createCanvas(S, S), mx2 = mc.getContext('2d'), img = mx2.createImageData(S, S);
  for (let p = 0; p < S * S; p++) { const v = Math.round(255 * (d[p] - lo) / (hi - lo || 1)); img.data[4 * p] = img.data[4 * p + 1] = img.data[4 * p + 2] = v; img.data[4 * p + 3] = 255; }
  mx2.putImageData(img, 0, 0);
  const W = src.width, H = src.height, big = E.createCanvas(W, H), bx = big.getContext('2d');
  bx.drawImage(mc, 0, 0, W, H);                                  // smooth upscale of the mask to the picture's size
  const m = bx.getImageData(0, 0, W, H).data, a = new Float32Array(W * H);
  for (let p = 0; p < W * H; p++) a[p] = m[4 * p] / 255;
  return a;
}
// mode 'cutout': the subject on white, in its own colours; 'silhouette': the subject in black on white.
async function cutoutPic(id, mode) {
  const held = pics.get(id);
  if (!held) throw new HttpError(410, 'picture ' + id + ' is not on the server');
  const { src } = held, W = src.width, H = src.height, a = await subjectMask(src), px = src.getContext('2d').getImageData(0, 0, W, H).data;
  const cv = E.createCanvas(W, H), cx = cv.getContext('2d'), img = cx.createImageData(W, H);
  for (let p = 0; p < W * H; p++) {
    for (let c = 0; c < 3; c++) {
      if (mode === 'silhouette') img.data[4 * p + c] = a[p] > 0.5 ? 0 : 255;
      else { const k = Math.min(1, Math.max(0, (a[p] - 0.35) / 0.3)); img.data[4 * p + c] = Math.round(px[4 * p + c] * k + 255 * (1 - k)); }
    }
    img.data[4 * p + 3] = 255;
  }
  cx.putImageData(img, 0, 0);
  return { id: await storePic(cv.toBuffer('image/png')) };
}

// A line drawing of the faces in a picture (optionally with the subject's outline), as a new picture to trace.
async function facePic(id, outline, sketch) {
  const held = pics.get(id);
  if (!held) throw new HttpError(410, 'picture ' + id + ' is not on the server');
  let v;
  try { v = await Face.faceVectors(held.src, outline ? held.alpha || await subjectMask(held.src) : null, sketch); }
  catch (e) { if (e instanceof HttpError) throw e; console.error(e); throw new HttpError(501, 'the face model is not installed on this server'); }
  if (!v) throw new HttpError(422, 'no face found in that picture');
  const r1 = (a) => a.map((x) => Math.round(x * 10) / 10), paths = { ox: r1(v.ox), oy: r1(v.oy) }, json = JSON.stringify(paths);
  const out = crypto.createHash('sha1').update(json).digest('hex').slice(0, 12);
  fs.writeFileSync(pathFile(out), json);
  holdPaths(out, paths);
  return { id: out, faces: v.faces };
}

/* ---------------------------------------------------------------- saved designs */
const DESIGN_ID = /^[0-9a-f]{8}$/, designFile = (id, ext) => path.join(DATA, 'designs', id + ext);
// Settings as plain data again, but only the page's own keys, after cleanP has vetted them.
function plainOf(raw) {
  const q = cleanP(raw), o = {};
  for (const k of Object.keys(E.DEFAULTS)) o[k] = q[k];
  if (q.pic) o.pic = { id: raw.pic.id, thr: q.pic.thr, detail: q.pic.detail, match: q.pic.match, clean: !!raw.pic.clean, name: q.pic.name };
  return o;
}
function listDesigns() {
  return fs.readdirSync(path.join(DATA, 'designs')).filter((f) => f.endsWith('.json')).map((f) => {
    try { const d = JSON.parse(fs.readFileSync(path.join(DATA, 'designs', f), 'utf8')); return { id: d.id, name: d.name, created: d.created }; } catch (_) { return null; }
  }).filter(Boolean).sort((a, b) => b.created - a.created);
}
async function saveDesign(b) {
  if (listDesigns().length >= 300) throw new HttpError(400, 'too many saved designs');
  await needPics(b.P, ...(Array.isArray(b.layers) ? b.layers : []));
  const d = { id: crypto.randomBytes(4).toString('hex'), name: safeName(b.name), created: Date.now(), vw: Math.round(num(b.vw, 200, 4000, 1280)), vh: Math.round(num(b.vh, 200, 4000, 800)),
    bg: /^#[0-9a-f]{6}$/i.test(b.bg) ? b.bg : '', P: plainOf(b.P), layers: (Array.isArray(b.layers) ? b.layers : []).slice(0, 24).map(plainOf) };
  const thumb = renderStill({ vw: d.vw, vh: d.vh, bg: '#282923', P: d.P, layers: d.layers }, 'jpg', 256);
  fs.writeFileSync(designFile(d.id, '.jpg'), thumb.buf);
  fs.writeFileSync(designFile(d.id, '.json'), JSON.stringify(d));
  return d;
}

// Pictures nobody saved a design with are dropped after two weeks.
(function prunePics() {
  const used = new Set();
  for (const f of fs.readdirSync(path.join(DATA, 'designs')).filter((x) => x.endsWith('.json'))) {
    try { const d = JSON.parse(fs.readFileSync(path.join(DATA, 'designs', f), 'utf8')); for (const p of [d.P, ...d.layers]) if (p.pic) used.add(p.pic.id); } catch (_) { /* skip */ }
  }
  for (const f of fs.readdirSync(path.join(DATA, 'pics'))) {
    const file = path.join(DATA, 'pics', f);
    if (!used.has(f.replace(/\.(lines|paths)$/, '')) && Date.now() - fs.statSync(file).mtimeMs > 14 * 864e5) fs.rmSync(file, { force: true });
  }
})();

/* ---------------------------------------------------------------- background jobs */
// Long renders and batches run one after another at low priority and are kept on disk; the page polls the list.
const jobs = new Map();                       // id -> { id, kind, label, fmt, state, created, finished, bytes, ext, error }
const JOB_ID = /^[0-9a-f]{10}$/, jobFile = (id, ext) => path.join(DATA, 'renders', id + ext);
let jobChain = Promise.resolve();
const saveJob = (j) => fs.writeFileSync(jobFile(j.id, '.json'), JSON.stringify(j));
const pubJob = (j) => { const { spec, ...rest } = j; return Object.assign(rest, { rerun: !!spec }); };       // the stored request stays on the server
for (const f of fs.readdirSync(path.join(DATA, 'renders')).filter((x) => x.endsWith('.json'))) {
  try {
    const j = JSON.parse(fs.readFileSync(path.join(DATA, 'renders', f), 'utf8'));
    if (j.state === 'queued' || j.state === 'running') { j.state = 'error'; j.error = 'server restarted'; saveJob(j); }
    jobs.set(j.id, j);
  } catch (_) { /* unreadable record: ignore */ }
}
function pruneJobs() {
  const all = [...jobs.values()].sort((a, b) => b.created - a.created);
  for (const j of all.slice(40).concat(all.filter((x) => Date.now() - x.created > 14 * 864e5))) {
    if (j.state === 'queued' || j.state === 'running') continue;
    jobs.delete(j.id);
    for (const e of ['.json', '.' + j.ext]) fs.rm(jobFile(j.id, e), () => {});
  }
}
async function notify(j) {
  const link = PUBLIC_URL ? PUBLIC_URL + '/api/jobs/' + j.id + '/file' : '', ok = j.state === 'done';
  const text = ok ? j.label + ' (' + j.ext.toUpperCase() + ', ' + (j.bytes / 1e6).toFixed(1) + ' MB) is ready' : j.label + ' failed: ' + j.error;
  const tasks = [];
  if (NTFY_URL) tasks.push(fetch(NTFY_URL, { method: 'POST', body: text, headers: Object.assign({ Title: 'Spirograph render ' + (ok ? 'done' : 'failed'), Tags: ok ? 'art' : 'warning' },
    link && ok ? { Click: link } : {}, NTFY_TOKEN ? { Authorization: 'Bearer ' + NTFY_TOKEN } : {}) }));
  if (HA_WEBHOOK_URL) tasks.push(fetch(HA_WEBHOOK_URL, { method: 'POST', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ id: j.id, state: j.state, kind: j.kind, fmt: j.fmt, label: j.label, bytes: j.bytes, url: link }) }));
  for (const r of await Promise.allSettled(tasks)) if (r.status === 'rejected' || !r.value.ok) console.error('notify failed', r.reason || r.value.status);
}
function startJob(b) {
  const kind = b.kind === 'batch' ? 'batch' : 'export', fmt = String(b.fmt);
  if (!(fmt in EXT) || (kind === 'batch' && !['png', 'jpg', 'svg', 'pdf', 'plotter'].includes(fmt))) throw new HttpError(400, 'bad format');
  const names = kind === 'batch' ? (Array.isArray(b.names) ? b.names : []).map((x) => String(x).trim()).filter(Boolean).slice(0, 100) : [];
  if (kind === 'batch' && !names.length) throw new HttpError(400, 'no names');
  const o = { long: true, size: b.size, fps: b.fps, seconds: b.seconds, page: String(b.page || 'a4'), margin: b.margin, cmyk: !!b.cmyk, colors: !!b.colors, pen: b.pen };
  const j = { id: crypto.randomBytes(5).toString('hex'), kind, fmt, label: safeName(b.label || (kind === 'batch' ? 'Batch' : 'Spirograph')), state: 'queued', created: Date.now(), ext: kind === 'batch' ? 'zip' : EXT[fmt], spec: b };
  o.job = j;
  jobs.set(j.id, j); saveJob(j);
  jobChain = jobChain.then(async () => {
    if (j.cancel) { j.state = 'cancelled'; j.finished = Date.now(); saveJob(j); return; }
    j.state = 'running'; j.started = Date.now(); saveJob(j);
    try {
      let buf;
      if (kind === 'batch') {
        const files = [];
        for (const [n, name] of names.entries()) {
          if (j.cancel) throw new HttpError(499, 'cancelled');
          j.done = n; j.total = names.length;
          await needPics(b.P);
          const sub = Object.assign({}, b, { P: Object.assign({}, b.P, { text: name, pic: undefined }), layers: [] });
          const r = await renderAny(sub, fmt, Object.assign({}, o, { maxPx: 4096 }));
          files.push({ name: String(n + 1).padStart(2, '0') + ' ' + safeName(name) + '.' + EXT[fmt], buf: r.buf });
          await new Promise((r2) => setImmediate(r2));
        }
        buf = zipFiles(files);
      } else {
        await needPics(b.P, ...(Array.isArray(b.layers) ? b.layers : []));
        buf = (await renderAny(b, fmt, o)).buf;
      }
      if (j.cancel) throw new HttpError(499, 'cancelled');
      fs.writeFileSync(jobFile(j.id, '.' + j.ext), buf);
      j.bytes = buf.length; j.state = 'done';
    } catch (e) {
      if (j.cancel) j.state = 'cancelled';
      else { console.error(e); j.state = 'error'; j.error = e instanceof HttpError ? e.message : String(e.message || e).slice(0, 200); }
    }
    j.finished = Date.now(); saveJob(j);
    console.log('job', j.id, j.kind, j.fmt, j.state, Math.round((j.finished - j.created) / 1000) + 's');
    if (j.state !== 'cancelled') notify(j).catch(() => {});
    pruneJobs();
  });
  return j;
}

/* ---------------------------------------------------------------- pattern of the day, by email */
// The same date always gives the same design: a seeded version of the page's Shuffle, one to three layers.
function rng(seed) {
  let a = seed >>> 0;
  return () => { a = (a + 0x6D2B79F5) >>> 0; let t = a; t = Math.imul(t ^ (t >>> 15), t | 1); t ^= t + Math.imul(t ^ (t >>> 7), t | 61); return ((t ^ (t >>> 14)) >>> 0) / 4294967296; };
}
function dailyDesign(date) {
  const R = rng([...date].reduce((h, c) => Math.imul(h ^ c.charCodeAt(0), 16777619), 2166136261)), pick = (a) => a[(R() * a.length) | 0], rnd = (a, b) => a + R() * (b - a);
  const inks = Object.keys(E.INKS).filter((k) => k !== 'custom'), out = [];
  for (let n = 1 + ((R() * 3) | 0); n--;) {
    const q = Object.assign({}, E.DEFAULTS);
    q.ringShape = pick(E.SHAPES); q.wheelShape = pick(E.SHAPES); q.side = R() < 0.5 ? 'inside' : 'outside';
    const wf = rnd(0.12, q.side === 'inside' ? 0.7 : 0.55);
    q.wheel = Math.round(wf * 100); q.ringTeeth = pick(E.RING_TEETH); q.wheelTeeth = pick(E.WHEEL_TEETH);
    q.pen = Math.round(rnd(30, 140)); q.angle = Math.round(rnd(0, 360));
    let ring = rnd(40, 85);
    if (q.side === 'outside') ring = Math.min(ring, 96 / (1 + wf * (1 + Math.max(q.pen / 100, 1))));
    q.ring = Math.max(20, Math.round(ring));
    if (R() < 0.2) { q.second = true; q.wheelShape = 'circle'; q.w2side = pick(['inside', 'outside']); q.w2div = 2 + ((R() * 7) | 0); q.w2speed = 1 + ((R() * 5) | 0); q.pen = Math.min(q.pen, 110); }
    q.ink = out.length ? pick(inks) : R() < 0.5 ? 'cycle' : pick(inks); q.hue = Math.round(rnd(0, 360)); q.weight = Math.round(rnd(0.8, 2.2) * 10) / 10;
    out.push(q);
  }
  return { vw: 1000, vh: 1000, bg: '#282923', P: out.pop(), layers: out };
}
const dayState = path.join(DATA, 'daily.json');
const today = (tz) => new Intl.DateTimeFormat('en-CA', { timeZone: tz || undefined }).format(new Date());            // 2026-10-24
const nowHM = (tz) => new Intl.DateTimeFormat('en-GB', { timeZone: tz || undefined, hour: '2-digit', minute: '2-digit', hour12: false }).format(new Date());
async function sendDaily(date, send = true) {
  const d = dailyDesign(date), design = await queue(async () => saveDesign(Object.assign({ name: 'Pattern of the day ' + date }, d)));
  const png = await queue(async () => renderStill(Object.assign({}, d), 'png', 1800)).then((r) => r.buf);
  const link = (PUBLIC_URL || '') + '/?d=' + design.id, name = 'Spirograph of the day ' + date;
  if (send) {
    if (!SMTP.host || !MAIL_TO) throw new HttpError(501, 'email is not configured on this server');
    const t = nodemailer.createTransport({ host: SMTP.host, port: SMTP.port, secure: SMTP.port === 465, auth: SMTP.user ? { user: SMTP.user, pass: SMTP.pass } : undefined });
    await t.sendMail({ from: MAIL_FROM || MAIL_TO, to: MAIL_TO, subject: name,
      text: name + '\n\nOpen it in the lab (home network or VPN): ' + link + '\n',
      html: '<p><img src="cid:pattern" alt="' + name + '" style="max-width:100%;border-radius:6px"></p><p><a href="' + link + '">Open it in the lab</a> (home network or VPN).</p>',
      attachments: [{ filename: name + '.png', content: png, cid: 'pattern' }] });
  }
  return { date, id: design.id, link, bytes: png.length, sent: send };
}
// Once a day at DAILY_AT (TZ), and again after a restart only if that day's mail has not gone out.
function dailyTimer() {
  if (!DAILY_AT) return;
  const tz = process.env.TZ || '';
  setInterval(async () => {
    const date = today(tz);
    let last = ''; try { last = JSON.parse(fs.readFileSync(dayState, 'utf8')).last; } catch (_) { /* first run */ }
    if (last === date || nowHM(tz) < DAILY_AT) return;
    fs.writeFileSync(dayState, JSON.stringify({ last: date }));          // before sending: a failure is retried tomorrow, not every 30 s
    try { console.log('daily', JSON.stringify(await sendDaily(date))); } catch (e) { console.error('daily failed', e); notify({ label: 'Pattern of the day', state: 'error', error: String(e.message || e).slice(0, 150), id: '', kind: 'daily', fmt: 'png', ext: 'png', bytes: 0 }).catch(() => {}); }
  }, 30e3).unref();
}

/* ---------------------------------------------------------------- picture from a URL */
async function fetchPicture(u) {
  let url;
  try { url = new URL(u); } catch (_) { throw new HttpError(400, 'bad url'); }
  if (url.protocol !== 'http:' && url.protocol !== 'https:') throw new HttpError(400, 'http(s) only');
  const r = await fetch(url, { signal: AbortSignal.timeout(20e3), redirect: 'follow' }).catch((e) => { throw new HttpError(502, 'fetch failed: ' + (e.cause && e.cause.code || e.message)); });
  if (!r.ok) throw new HttpError(502, 'fetch failed: HTTP ' + r.status);
  if (+r.headers.get('content-length') > MAX_BODY) throw new HttpError(413, 'picture too large');
  const bytes = Buffer.from(await r.arrayBuffer());
  if (bytes.length > MAX_BODY) throw new HttpError(413, 'picture too large');
  return { id: await storePic(bytes), name: decodeURIComponent(url.pathname.split('/').pop() || 'picture').replace(/\.[^.]*$/, '').slice(0, 60) };
}

/* ---------------------------------------------------------------- http */
const MIME = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript', '.png': 'image/png', '.svg': 'image/svg+xml', '.woff2': 'font/woff2',
  '.webmanifest': 'application/manifest+json', '.txt': 'text/plain', '.xml': 'application/xml' };
const CSP = "default-src 'self'; style-src 'self' 'unsafe-inline'; script-src 'self' 'unsafe-inline'; img-src 'self' data:";
const SEC = { 'X-Content-Type-Options': 'nosniff', 'Referrer-Policy': 'no-referrer', 'Content-Security-Policy': CSP, 'Cache-Control': 'no-store' };
// Search engines have no business here: it is the private build.
const PAGE = () => fs.readFileSync(path.join(SITE, 'index.html'), 'utf8')
  .replace('</head>', '<meta name="robots" content="noindex, nofollow">\n<script>window.SPIRO_SERVER = true;</script>\n</head>');

function body(req, limit = MAX_BODY) {
  return new Promise((resolve, reject) => {
    const parts = []; let n = 0;
    req.on('data', (c) => { n += c.length; if (n > limit) { reject(new HttpError(413, 'body too large')); req.destroy(); } else parts.push(c); });
    req.on('end', () => resolve(Buffer.concat(parts)));
    req.on('error', reject);
  });
}
const send = (res, code, data, type = 'text/plain', extra = {}) => { res.writeHead(code, Object.assign({ 'Content-Type': type }, SEC, extra)); res.end(data); };
const json = (res, code, o) => send(res, code, JSON.stringify(o), 'application/json');

async function api(req, res, url) {
  const p = url.pathname, M = req.method, jbody = async (lim) => JSON.parse((await body(req, lim)).toString() || '{}');
  if (M === 'POST' && p === '/api/terms/name') {
    const lines = (await jbody(20e3)).lines;
    if (!Array.isArray(lines) || !lines.length || lines.length > 8 || lines.some((l) => typeof l !== 'string' || !l.trim() || l.length > E.MAX_TEXT)) throw new HttpError(400, 'bad lines');
    const buf = await queue(() => nameTerms(lines));
    return buf.length ? send(res, 200, buf, 'application/octet-stream') : send(res, 204, '');
  }
  if (M === 'POST' && p === '/api/terms/pic') {
    const bytes = await body(req);
    let id = url.searchParams.get('id') || '';
    if (bytes.length) id = await queue(() => storePic(bytes));
    const thr = Math.round(num(url.searchParams.get('thr'), 5, 95, 30)), detail = Math.round(num(url.searchParams.get('detail'), 96, 4096, 320));
    const buf = await queue(async () => { await needPics({ pic: { id } }); return picTerms(id, thr, detail, url.searchParams.get('clean') === '1'); });
    return buf.length ? send(res, 200, buf, 'application/octet-stream', { 'X-Pic-Id': id }) : send(res, 204, '', 'text/plain', { 'X-Pic-Id': id });
  }
  if (M === 'POST' && p === '/api/pic/split') {
    const id = url.searchParams.get('id') || '', k = Math.round(num(url.searchParams.get('k'), 2, 8, 4));
    const out = await queue(async () => { await needPics({ pic: { id } }); return splitPic(id, k); });
    return json(res, 200, out);
  }
  if (M === 'POST' && p === '/api/pic/cutout') {
    const id = url.searchParams.get('id') || '', mode = url.searchParams.get('mode') === 'silhouette' ? 'silhouette' : 'cutout';
    return json(res, 200, await queue(async () => { await needPics({ pic: { id } }); return cutoutPic(id, mode); }));
  }
  if (M === 'POST' && p === '/api/pic/face') {
    const id = url.searchParams.get('id') || '', outline = url.searchParams.get('outline') === '1', sketch = url.searchParams.get('sketch') === '1';
    return json(res, 200, await queue(async () => { await needPics({ pic: { id } }); return facePic(id, outline, sketch); }));
  }
  if (M === 'POST' && p === '/api/daily/run') {                       // ?date=YYYY-MM-DD, ?send=0 for a preview without email
    const date = url.searchParams.get('date') || today(process.env.TZ || '');
    if (!/^\d{4}-\d{2}-\d{2}$/.test(date)) throw new HttpError(400, 'bad date');
    return json(res, 200, await sendDaily(date, url.searchParams.get('send') !== '0'));
  }
  if (M === 'POST' && p === '/api/fetch') {
    const j = await jbody(4e3), r = await queue(() => fetchPicture(String(j.url || '')));
    return json(res, 200, r);
  }
  if (M === 'POST' && p === '/api/export') {
    const b = await jbody(2e6), fmt = String(b.fmt);
    if (!['png', 'jpg', 'svg', 'gif', 'mp4', 'pdf', 'plotter'].includes(fmt)) throw new HttpError(400, 'bad format');
    const t0 = Date.now();
    const r = await queue(async () => { await needPics(b.P, ...(Array.isArray(b.layers) ? b.layers : [])); return renderAny(b, fmt, b.opt || {}); });
    console.log('export', fmt, r.buf.length, 'bytes', Date.now() - t0, 'ms');
    return send(res, 200, r.buf, r.type);
  }
  if (p === '/api/designs') {
    if (M === 'GET') return json(res, 200, listDesigns());
    if (M === 'POST') { const d = await queue(async () => saveDesign(await jbody(2e6))); return json(res, 200, { id: d.id, name: d.name }); }
  }
  let m = /^\/api\/designs\/([0-9a-f]{8})(\/thumb)?$/.exec(p);
  if (m) {
    const f = designFile(m[1], m[2] ? '.jpg' : '.json');
    if (M === 'GET') {
      const data = await fs.promises.readFile(f).catch(() => { throw new HttpError(404, 'no such design'); });
      return send(res, 200, data, m[2] ? 'image/jpeg' : 'application/json');
    }
    if (M === 'DELETE' && !m[2]) { for (const e of ['.json', '.jpg']) fs.rmSync(designFile(m[1], e), { force: true }); return json(res, 200, { ok: true }); }
  }
  if (p === '/api/jobs') {
    if (M === 'GET') return json(res, 200, [...jobs.values()].sort((a, b) => b.created - a.created).map(pubJob));
    if (M === 'POST') return json(res, 200, pubJob(startJob(await jbody(2e6))));
  }
  m = /^\/api\/jobs\/([0-9a-f]{10})(\/file|\/cancel|\/rerun)?$/.exec(p);
  if (m && jobs.has(m[1])) {
    const j = jobs.get(m[1]);
    if (M === 'POST' && m[2] === '/cancel') {                       // a queued job never starts; a running one stops at its next frame or item
      if (j.state !== 'queued' && j.state !== 'running') throw new HttpError(409, 'already finished');
      j.cancel = true;
      if (j.state === 'queued') { j.state = 'cancelled'; j.finished = Date.now(); saveJob(j); }
      return json(res, 200, pubJob(j));
    }
    if (M === 'POST' && m[2] === '/rerun') {                       // the same request again, with any export options the caller changes
      if (!j.spec) throw new HttpError(409, 'this job was made before reruns existed');
      const ov = await jbody(1e4), keep = ['fmt', 'size', 'fps', 'seconds', 'page', 'margin', 'cmyk', 'colors', 'pen', 'label'], spec = Object.assign({}, j.spec);
      for (const k of keep) if (ov[k] !== undefined) spec[k] = ov[k];
      return json(res, 200, pubJob(startJob(spec)));
    }
    if (M === 'GET' && m[2] === '/file') {
      if (j.state !== 'done') throw new HttpError(409, 'not finished');
      const type = { mp4: 'video/mp4', gif: 'image/gif', png: 'image/png', jpg: 'image/jpeg', svg: 'image/svg+xml', pdf: 'application/pdf', zip: 'application/zip' }[j.ext];
      const data = await fs.promises.readFile(jobFile(j.id, '.' + j.ext)).catch(() => { throw new HttpError(404, 'file is gone'); });
      return send(res, 200, data, type, { 'Content-Disposition': "attachment; filename*=UTF-8''" + encodeURIComponent(j.label + ' - Spirograph.' + j.ext) });
    }
    if (M === 'GET') return json(res, 200, pubJob(j));
    if (M === 'DELETE') {
      if (j.state === 'queued' || j.state === 'running') throw new HttpError(409, 'still running');
      jobs.delete(j.id);
      for (const e of ['.json', '.' + j.ext]) fs.rmSync(jobFile(j.id, e), { force: true });
      return json(res, 200, { ok: true });
    }
  }
  throw new HttpError(404, 'no such endpoint');
}

const server = http.createServer(async (req, res) => {
  try {
    const url = new URL(req.url, 'http://x');
    if (url.pathname === '/healthz') return send(res, 200, 'ok\n');
    if (url.pathname.startsWith('/api/')) return await api(req, res, url);
    if (req.method !== 'GET' && req.method !== 'HEAD') throw new HttpError(405, 'method not allowed');
    let name = decodeURIComponent(url.pathname);
    if (name === '/' || name === '/index.html') return send(res, 200, PAGE(), MIME['.html']);
    if (name === '/robots.txt') return send(res, 200, 'User-agent: *\nDisallow: /\n');
    if (name === '/3d') return send(res, 200, fs.readFileSync(path.join(SITE, '3d.html'), 'utf8').replace('</head>', '<meta name="robots" content="noindex, nofollow">\n</head>'), MIME['.html']);   // the private build is not indexed
    const file = path.normalize(path.join(SITE, name));
    if (!file.startsWith(SITE + path.sep) || name === '/_headers' || name === '/sw.js') throw new HttpError(404, 'not found');
    const data = await fs.promises.readFile(file).catch(() => { throw new HttpError(404, 'not found'); });
    return send(res, 200, data, MIME[path.extname(file)] || 'application/octet-stream');
  } catch (e) {
    if (!(e instanceof HttpError)) console.error(e);
    if (!res.headersSent) json(res, e instanceof HttpError ? e.code : e instanceof SyntaxError ? 400 : 500, { error: e instanceof HttpError || e instanceof SyntaxError ? e.message : 'server error' });
  }
});
if (require.main === module) server.listen(PORT, () => { console.log('spirograph server on :' + PORT); dailyTimer(); });
module.exports = { server, packTerms, cleanP, subjectMask };
