// Homelab build of Spirograph. Serves the same page as the public site, plus an API that does the heavy work:
// tracing names and uploaded pictures into Fourier terms, and rendering PNG, JPG, SVG, GIF and MP4 exports.
// The page's engine (geometry, FFT, tracing, stroking) is the page's own code, run here by ./engine.js.
const http = require('http');
const fs = require('fs');
const os = require('os');
const path = require('path');
const crypto = require('crypto');
const { spawn } = require('child_process');
const E = require('./engine');
const { loadImage } = require('@napi-rs/canvas');

const SITE = process.env.SITE_DIR || path.join(__dirname, '..', 'site');
const PORT = +process.env.PORT || 8080;
const MAX_BODY = 30e6;
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
const pics = new Lru(8);            // id -> { src canvas }: the decoded picture, kept so sliders and exports don't re-upload it
const picTermsCache = new Lru(16);  // id|thr|detail -> packed terms
async function storePic(bytes) {
  const id = crypto.createHash('sha1').update(bytes).digest('hex').slice(0, 12);
  if (pics.get(id)) return id;
  let img;
  try { img = await loadImage(bytes); } catch (_) { throw new HttpError(415, 'That file could not be read as an image'); }
  const k = Math.min(1, 2048 / Math.max(img.width, img.height)), src = E.createCanvas(Math.max(1, Math.round(img.width * k)), Math.max(1, Math.round(img.height * k)));
  const c = src.getContext('2d');
  c.fillStyle = '#fff'; c.fillRect(0, 0, src.width, src.height);                // transparent areas read as white
  c.drawImage(img, 0, 0, src.width, src.height);
  pics.set(id, { src });
  return id;
}
function picObject(m) {                // a client's picture description -> what the engine's picTerms expects
  const held = m && typeof m.id === 'string' && pics.get(m.id);
  if (!held) throw new HttpError(410, 'picture ' + (m && m.id) + ' is not on the server');
  return { src: held.src, name: String(m.name || '').slice(0, 60), thr: num(m.thr, 5, 95, 30), detail: Math.round(num(m.detail, 96, 1536, 320)),
    match: !!m.match, key: '', T: null };
}
function picTerms(id, thr, detail) {
  const key = id + '|' + thr + '|' + detail;
  let buf = picTermsCache.get(key);
  if (!buf) {
    const T = E.picTerms(picObject({ id, thr, detail, match: true }));
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
function renderStill(body, fmt) {
  const { vw, vh, layers, cur } = models(body);
  const all = layers.concat([cur]), R = region(bounds(all), vw, vh);
  // up to 8x the screen, 8192 px on the long side, 36 megapixels
  const scale = Math.min(8, 8192 / Math.max(R.w, R.h), Math.sqrt(36e6 / (R.w * R.h)));
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
function renderSVG(body) {
  const { vw, vh, layers, cur } = models(body), all = layers.concat([cur]), R = region(bounds(all), vw, vh), out = [];
  for (const m of all) {
    const match = E.matching(m), step = match ? Math.ceil(m.N / 400) : m.p.ink === 'cycle' ? Math.ceil(m.N / 90) : m.N;
    for (let i = 0; i < m.N; i += step) {
      const e = Math.min(m.N, i + step), c = match ? E.picColor(m, i, e) : E.inkRGB(m.p, i / m.N);
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
function ffmpeg(args, frames, ext) {
  const file = path.join(os.tmpdir(), 'spiro-' + crypto.randomBytes(6).toString('hex') + '.' + ext);
  return new Promise((resolve, reject) => {
    const ff = spawn('ffmpeg', ['-hide_banner', '-loglevel', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'rgba', ...args, file], { stdio: ['pipe', 'ignore', 'pipe'] });
    let err = '';
    const kill = setTimeout(() => ff.kill('SIGKILL'), 240e3);
    ff.stderr.on('data', (d) => { err += d; });
    ff.on('error', (e) => { clearTimeout(kill); reject(e); });
    ff.on('close', (code) => {
      clearTimeout(kill);
      if (code) { fs.rm(file, () => {}); return reject(new Error('ffmpeg exit ' + code + ': ' + err.slice(-300))); }
      const buf = fs.readFileSync(file); fs.rm(file, () => {});
      resolve(buf);
    });
    ff.stdin.on('error', () => {});
    (async () => {
      for (let f = 0; f < frames.count; f++) {
        const fr = frames.at(f), px = Buffer.from(fr.buffer, fr.byteOffset, fr.length);
        if (!ff.stdin.write(px)) await new Promise((r) => ff.stdin.once('drain', r));
      }
      ff.stdin.end();
    })().catch(reject);
  });
}
async function renderAnimation(body, fmt) {
  const gif = fmt === 'gif', side = gif ? 480 : 1080, fps = gif ? 25 : 30, seconds = gif ? 6 : Math.round(num(body.seconds, 3, 20, 9)), hold = Math.round(fps * 1.5);
  const src = frameSource(body, side), draw = fps * seconds, total = draw + hold;
  const frames = { count: total, at: (f) => src.frame(Math.round(src.n * Math.min(1, (f + 1) / draw))) };
  const size = side + 'x' + side;
  if (gif) {
    const buf = await ffmpeg(['-s', size, '-r', String(fps), '-i', '-', '-vf', 'split[a][b];[a]palettegen=stats_mode=diff[p];[b][p]paletteuse=dither=bayer:bayer_scale=5:diff_mode=rectangle', '-loop', '0'], frames, 'gif');
    return { type: 'image/gif', buf };
  }
  const buf = await ffmpeg(['-s', size, '-r', String(fps), '-i', '-', '-c:v', 'libx264', '-preset', 'medium', '-crf', '17', '-pix_fmt', 'yuv420p', '-movflags', '+faststart'], frames, 'mp4');
  return { type: 'video/mp4', buf };
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
  if (req.method !== 'POST') throw new HttpError(405, 'POST only');
  if (url.pathname === '/api/terms/name') {
    const j = JSON.parse((await body(req, 20e3)).toString());
    const lines = j.lines;
    if (!Array.isArray(lines) || !lines.length || lines.length > 8 || lines.some((l) => typeof l !== 'string' || !l.trim() || l.length > E.MAX_TEXT)) throw new HttpError(400, 'bad lines');
    const buf = await queue(() => nameTerms(lines));
    return buf.length ? send(res, 200, buf, 'application/octet-stream') : send(res, 204, '');
  }
  if (url.pathname === '/api/terms/pic') {
    const bytes = await body(req);
    let id = url.searchParams.get('id') || '';
    if (bytes.length) id = await queue(() => storePic(bytes));
    const thr = Math.round(num(url.searchParams.get('thr'), 5, 95, 30)), detail = Math.round(num(url.searchParams.get('detail'), 96, 1536, 320));
    const buf = await queue(() => picTerms(id, thr, detail));
    return buf.length ? send(res, 200, buf, 'application/octet-stream', { 'X-Pic-Id': id }) : send(res, 204, '', 'text/plain', { 'X-Pic-Id': id });
  }
  if (url.pathname === '/api/export') {
    const b = JSON.parse((await body(req, 2e6)).toString()), fmt = String(b.fmt);
    if (!['png', 'jpg', 'svg', 'gif', 'mp4'].includes(fmt)) throw new HttpError(400, 'bad format');
    const t0 = Date.now();
    const r = await queue(() => (fmt === 'gif' || fmt === 'mp4' ? renderAnimation(b, fmt) : fmt === 'svg' ? renderSVG(b) : renderStill(b, fmt)));
    console.log('export', fmt, r.buf.length, 'bytes', Date.now() - t0, 'ms');
    return send(res, 200, r.buf, r.type);
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
    const file = path.normalize(path.join(SITE, name));
    if (!file.startsWith(SITE + path.sep) || name === '/_headers' || name === '/sw.js') throw new HttpError(404, 'not found');
    const data = await fs.promises.readFile(file).catch(() => { throw new HttpError(404, 'not found'); });
    return send(res, 200, data, MIME[path.extname(file)] || 'application/octet-stream');
  } catch (e) {
    if (!(e instanceof HttpError)) console.error(e);
    if (!res.headersSent) json(res, e instanceof HttpError ? e.code : e instanceof SyntaxError ? 400 : 500, { error: e instanceof HttpError || e instanceof SyntaxError ? e.message : 'server error' });
  }
});
if (require.main === module) server.listen(PORT, () => console.log('spirograph server on :' + PORT));
module.exports = { server, packTerms, cleanP };
