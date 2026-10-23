// Face finding and 106-point landmarks (InsightFace det_500m + 2d106det, run with onnxruntime). Personal use only:
// InsightFace's pretrained models are licensed for non-commercial research.
const path = require('path');
const E = require('./engine');

const DIR = process.env.FACE_MODEL_DIR || path.join(__dirname, 'models');
let nets = null;
async function load() {
  if (nets) return nets;
  const ort = require('onnxruntime-node'), opt = { intraOpNumThreads: 4 };
  nets = { ort, det: await ort.InferenceSession.create(path.join(DIR, 'det_500m.onnx'), opt), lm: await ort.InferenceSession.create(path.join(DIR, '2d106det.onnx'), opt) };
  return nets;
}

// SCRFD: boxes for every face in the picture (src: a canvas or image).
async function detect(src, thresh = 0.5) {
  const { ort, det } = await load(), S = 640, W = src.width, H = src.height, k = Math.min(S / W, S / H);
  const cv = E.createCanvas(S, S), cx = cv.getContext('2d');
  cx.fillStyle = '#000'; cx.fillRect(0, 0, S, S);
  cx.drawImage(src, 0, 0, Math.round(W * k), Math.round(H * k));
  const px = cx.getImageData(0, 0, S, S).data, f = new Float32Array(3 * S * S);
  for (let p = 0; p < S * S; p++) for (let c = 0; c < 3; c++) f[c * S * S + p] = (px[4 * p + c] - 127.5) / 128;
  const out = await det.run({ [det.inputNames[0]]: new ort.Tensor('float32', f, [1, 3, S, S]) }), o = det.outputNames.map((n) => out[n].data), boxes = [];
  [8, 16, 32].forEach((stride, i) => {
    const scores = o[i], bb = o[i + 3], kp = o[i + 6], cols = S / stride;
    for (let n = 0; n < scores.length; n++) {
      if (scores[n] < thresh) continue;
      const a = (n / 2) | 0, x = (a % cols) * stride, y = ((a / cols) | 0) * stride;
      boxes.push({ score: scores[n], x1: (x - bb[4 * n] * stride) / k, y1: (y - bb[4 * n + 1] * stride) / k, x2: (x + bb[4 * n + 2] * stride) / k, y2: (y + bb[4 * n + 3] * stride) / k });
    }
  });
  boxes.sort((a, b) => b.score - a.score);
  const keep = [];                                                  // non-maximum suppression
  for (const b of boxes) {
    if (keep.every((q) => {
      const iw = Math.min(b.x2, q.x2) - Math.max(b.x1, q.x1), ih = Math.min(b.y2, q.y2) - Math.max(b.y1, q.y1);
      if (iw <= 0 || ih <= 0) return true;
      const inter = iw * ih;
      return inter / ((b.x2 - b.x1) * (b.y2 - b.y1) + (q.x2 - q.x1) * (q.y2 - q.y1) - inter) < 0.4;
    })) keep.push(b);
  }
  return keep;
}

// 106 points for one box, in the picture's own pixels.
async function landmarks(src, box, norm = 'raw') {
  const { ort, lm } = await load(), S = 192, w = box.x2 - box.x1, h = box.y2 - box.y1, cx0 = (box.x1 + box.x2) / 2, cy0 = (box.y1 + box.y2) / 2, sc = S / (Math.max(w, h) * 1.5);
  const cv = E.createCanvas(S, S), cx = cv.getContext('2d');
  cx.setTransform(sc, 0, 0, sc, S / 2 - sc * cx0, S / 2 - sc * cy0);
  cx.drawImage(src, 0, 0);
  const px = cx.getImageData(0, 0, S, S).data, f = new Float32Array(3 * S * S);
  for (let p = 0; p < S * S; p++) for (let c = 0; c < 3; c++) f[c * S * S + p] = norm === 'raw' ? px[4 * p + c] : (px[4 * p + c] - 127.5) / 128;
  const out = await lm.run({ [lm.inputNames[0]]: new ort.Tensor('float32', f, [1, 3, S, S]) }), d = out[lm.outputNames[0]].data, pts = [];
  for (let i = 0; i < 106; i++) pts.push([cx0 + ((d[2 * i] + 1) * S / 2 - S / 2) / sc, cy0 + ((d[2 * i + 1] + 1) * S / 2 - S / 2) / sc]);
  return pts;
}

/* ---------------------------------------------------------------- line drawing of the faces */
// 106-point layout (InsightFace 2d106det): contour 0-32 (listed here in walking order), left eye 33-42 and brow 43-51 (the
// viewer's left), right eye 87-96 and brow 97-105, nose 72-86, mouth 52-71.
const JAW = [1, 9, 10, 11, 12, 13, 14, 15, 16, 2, 3, 4, 5, 6, 7, 8, 0, 24, 23, 22, 21, 20, 19, 18, 32, 31, 30, 29, 28, 27, 26, 25, 17];
const range = (a, b) => Array.from({ length: b - a + 1 }, (_, i) => a + i);
const centre = (pts) => [pts.reduce((s, p) => s + p[0], 0) / pts.length, pts.reduce((s, p) => s + p[1], 0) / pts.length];
const byAngle = (pts) => { const [cx, cy] = centre(pts); return pts.slice().sort((p, q) => Math.atan2(p[1] - cy, p[0] - cx) - Math.atan2(q[1] - cy, q[0] - cx)); };
function hull(pts) {                                              // convex hull, counter-clockwise
  const p = pts.slice().sort((a, b) => a[0] - b[0] || a[1] - b[1]), cr = (o, a, b) => (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]), lo = [], up = [];
  for (const q of p) { while (lo.length > 1 && cr(lo[lo.length - 2], lo[lo.length - 1], q) <= 0) lo.pop(); lo.push(q); }
  for (const q of p.slice().reverse()) { while (up.length > 1 && cr(up[up.length - 2], up[up.length - 1], q) <= 0) up.pop(); up.push(q); }
  return lo.slice(0, -1).concat(up.slice(0, -1));
}
// Points binned along x: per bin the middle, topmost or bottommost point; the end bins are the extreme points.
function bins(pts, n, how) {
  const xs = pts.map((p) => p[0]), lo = Math.min(...xs), hi = Math.max(...xs), out = [];
  for (let i = 0; i < n; i++) {
    const a = lo + (hi - lo) * i / n, b = lo + (hi - lo) * (i + 1) / n, g = pts.filter((p) => p[0] >= a && (p[0] < b || i === n - 1));
    if (!g.length) continue;
    const x = g.reduce((s, p) => s + p[0], 0) / g.length, ys = g.map((p) => p[1]);
    out.push([x, how === 'min' ? Math.min(...ys) : how === 'max' ? Math.max(...ys) : ys.reduce((s, v) => s + v, 0) / ys.length]);
  }
  return out;
}
// A smooth curve through the points (quadratics between midpoints).
function curve(c, pts, closed) {
  if (pts.length < 2) return;
  c.beginPath();
  if (closed) {
    const m = (a, b) => [(a[0] + b[0]) / 2, (a[1] + b[1]) / 2], n = pts.length, s = m(pts[n - 1], pts[0]);
    c.moveTo(s[0], s[1]);
    for (let i = 0; i < n; i++) { const e = m(pts[i], pts[(i + 1) % n]); c.quadraticCurveTo(pts[i][0], pts[i][1], e[0], e[1]); }
    c.closePath();
  } else {
    c.moveTo(pts[0][0], pts[0][1]);
    for (let i = 1; i < pts.length - 1; i++) c.quadraticCurveTo(pts[i][0], pts[i][1], (pts[i][0] + pts[i + 1][0]) / 2, (pts[i][1] + pts[i + 1][1]) / 2);
    c.lineTo(pts[pts.length - 1][0], pts[pts.length - 1][1]);
  }
  c.stroke();
}
// Black lines on white, cropped around the faces (long side 640). mask (optional): subject mask over src (1 = subject), drawn as an
// outline. sketch: also keep the photo's strongest edges where there is detail worth keeping (hair, ears, glasses, around the eyes),
// but not the smooth skin of the face, which stays clean.
async function faceArt(src, mask, sketch = false) {
  let boxes = await detect(src);
  if (!boxes.length) return null;
  boxes = boxes.slice(0, 6);
  const all = [];
  for (const b of boxes) all.push(await landmarks(src, b));
  let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity;
  for (const b of boxes) { x0 = Math.min(x0, b.x1); y0 = Math.min(y0, b.y1); x1 = Math.max(x1, b.x2); y1 = Math.max(y1, b.y2); }
  const fw = x1 - x0, fh = y1 - y0, W = src.width, H = src.height;
  let rx0 = x0 - fw * 0.25, ry0 = y0 - fh * 0.25, rw = fw * 1.5, rh = fh * 1.5;
  if (mask) {                                                       // frame the whole subject (hair, shoulders), not just the face
    let a0 = W, b0 = H, a1 = 0, b1 = 0;
    for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) if (mask[y * W + x] > 0.5) { if (x < a0) a0 = x; if (x > a1) a1 = x; if (y < b0) b0 = y; if (y > b1) b1 = y; }
    if (a1 > a0 && b1 > b0) { const m = Math.max(a1 - a0, b1 - b0) * 0.05; rx0 = a0 - m; ry0 = b0 - m; rw = a1 - a0 + 2 * m; rh = b1 - b0 + 2 * m; }
  }
  const k = 640 / Math.max(rw, rh), cw = Math.round(rw * k), ch = Math.round(rh * k), X = (x) => (x - rx0) * k, Y = (y) => (y - ry0) * k, cv = E.createCanvas(cw, ch), c = cv.getContext('2d');
  c.fillStyle = '#fff'; c.fillRect(0, 0, cw, ch);
  c.strokeStyle = '#000'; c.fillStyle = '#000'; c.lineWidth = 3; c.lineJoin = c.lineCap = 'round';
  const P = (L, ids) => ids.map((i) => [X(L[i][0]), Y(L[i][1])]);
  // zones in crop pixels: the face box's skin (kept clean) and the eye regions (kept detailed, for glasses and lids)
  const skin = boxes.map((b) => ({ x0: X(b.x1), y0: Y(b.y1), x1: X(b.x2), y1: Y(b.y2) }));
  const eyeZone = all.flatMap((L) => [[33, 42], [87, 96]].map(([a, b]) => {
    const e = P(L, range(a, b)), ex = e.map((p) => p[0]), ey = e.map((p) => p[1]), w = Math.max(...ex) - Math.min(...ex);
    return { x0: Math.min(...ex) - w * 1.0, x1: Math.max(...ex) + w * 1.0, y0: Math.min(...ey) - w * 1.0, y1: Math.max(...ey) + w * 1.1 };
  }));
  const within = (z, x, y) => x >= z.x0 && x <= z.x1 && y >= z.y0 && y <= z.y1;
  const inside = new Uint8Array(cw * ch);
  for (let y = 0; y < ch; y++) for (let x = 0; x < cw; x++) {
    const sx = Math.min(W - 1, Math.max(0, Math.round(rx0 + x / k))), sy = Math.min(H - 1, Math.max(0, Math.round(ry0 + y / k)));
    if (mask) inside[y * cw + x] = mask[sy * W + sx] > 0.5 ? 1 : 0;
    else inside[y * cw + x] = skin.some((z) => within({ x0: z.x0 - (z.x1 - z.x0) * 0.4, x1: z.x1 + (z.x1 - z.x0) * 0.4, y0: z.y0 - (z.y1 - z.y0) * 0.4, y1: z.y1 + (z.y1 - z.y0) * 0.4 }, x, y)) ? 1 : 0;
  }
  if (mask) {                                                       // the subject's outline: smoothed so it is one clean line
    const sm = new Float32Array(cw * ch);
    for (let pass = 0; pass < 2; pass++) {
      const from = pass ? sm.slice() : Float32Array.from(inside);
      for (let y = 2; y < ch - 2; y++) for (let x = 2; x < cw - 2; x++) {
        let t = 0;
        for (let dy = -2; dy <= 2; dy++) for (let dx = -2; dx <= 2; dx++) t += from[(y + dy) * cw + x + dx];
        sm[y * cw + x] = t / 25;
      }
    }
    for (let y = 1; y < ch - 1; y++) for (let x = 1; x < cw - 1; x++) {
      const i = y * cw + x;
      if (sm[i] > 0.5 && (sm[i - 1] <= 0.5 || sm[i + 1] <= 0.5 || sm[i - cw] <= 0.5 || sm[i + cw] <= 0.5)) c.fillRect(x - 0.8, y - 0.8, 2.2, 2.2);
    }
  }
  if (sketch) {                                                     // the photo's edges (Canny), off the clean skin and away from the drawn eyes and brows
    const g = E.createCanvas(cw, ch), gx = g.getContext('2d', { willReadFrequently: true });
    gx.filter = 'grayscale(1) blur(1.4px)';
    gx.fillStyle = '#fff'; gx.fillRect(0, 0, cw, ch);
    gx.drawImage(src, rx0, ry0, rw, rh, 0, 0, cw, ch);
    const d = gx.getImageData(0, 0, cw, ch).data, N = cw * ch, mag = new Float32Array(N), dir = new Uint8Array(N), keep = new Uint8Array(N), zone = new Uint8Array(N);
    const own = all.flatMap((L) => [[33, 42], [87, 96], [43, 51], [97, 105]].map(([a, b]) => {                    // where the landmark eyes and brows go
      const e = P(L, range(a, b)), ex = e.map((p) => p[0]), ey = e.map((p) => p[1]), w = Math.max(...ex) - Math.min(...ex);
      return { x0: Math.min(...ex) - w * 0.04, x1: Math.max(...ex) + w * 0.04, y0: Math.min(...ey) - w * 0.04, y1: Math.max(...ey) + w * 0.04 };
    }));
    for (let y = 2; y < ch - 2; y++) for (let x = 2; x < cw - 2; x++) {
      const i = y * cw + x, v = (j) => d[4 * j];
      const sx = v(i - cw + 1) + 2 * v(i + 1) + v(i + cw + 1) - v(i - cw - 1) - 2 * v(i - 1) - v(i + cw - 1), sy = v(i + cw - 1) + 2 * v(i + cw) + v(i + cw + 1) - v(i - cw - 1) - 2 * v(i - cw) - v(i - cw + 1);
      mag[i] = Math.hypot(sx, sy);
      const ang = (Math.atan2(sy, sx) + Math.PI) / Math.PI * 4;       // 0..8, in units of 45 degrees
      dir[i] = Math.round(ang) % 4;                                   // 0: horizontal gradient, 1: diagonal, 2: vertical, 3: other diagonal
      if (inside[i] && !own.some((z) => within(z, x, y)) && (eyeZone.some((z) => within(z, x, y)) || !skin.some((z) => within(z, x, y)))) keep[i] = 1;
      zone[i] = eyeZone.some((z) => within(z, x, y)) ? 1 : 0;
    }
    const step = [[1, 0], [1, 1], [0, 1], [-1, 1]], thin = new Float32Array(N);       // non-maximum suppression along the gradient
    for (let y = 2; y < ch - 2; y++) for (let x = 2; x < cw - 2; x++) {
      const i = y * cw + x, [dx, dy] = step[dir[i]];
      if (keep[i] && mag[i] >= mag[i + dy * cw + dx] && mag[i] >= mag[i - dy * cw - dx]) thin[i] = mag[i];
    }
    const hi = (zn) => { const v = []; for (let i = 0; i < N; i += 2) if (keep[i] && zone[i] === zn && thin[i] > 0) v.push(thin[i]); v.sort((p, q) => p - q); return v.length ? v[Math.floor(v.length * (zn ? 0.78 : 0.96))] : 1e9; };
    const hiT = [hi(0), hi(1)], strong = [], mark = new Uint8Array(N);                   // hysteresis: strong edges, plus the weaker ones joined to them
    for (let i = 0; i < N; i++) if (thin[i] > hiT[zone[i]]) { mark[i] = 1; strong.push(i); }
    const comp = [];
    for (const s0 of strong) {
      if (mark[s0] === 2) continue;
      const list = [s0]; mark[s0] = 2;
      for (let h = 0; h < list.length; h++) for (let dy = -1; dy <= 1; dy++) for (let dx = -1; dx <= 1; dx++) {
        const q = list[h] + dy * cw + dx;
        if (q >= 0 && q < N && mark[q] !== 2 && thin[q] > hiT[zone[q]] * 0.5) { mark[q] = 2; list.push(q); }
      }
      comp.push(list);
    }
    for (const list of comp) if (list.length >= 40) for (const i of list) c.fillRect(i % cw, (i / cw) | 0, 1.4, 1.4);       // short scraps are noise
  }
  for (const L of all) {
    if (!mask) curve(c, P(L, JAW), false);                           // no subject outline: the jaw from the landmarks
    for (const [a, b] of [[43, 51], [97, 105]]) { c.lineWidth = 4; curve(c, bins(P(L, range(a, b)), 6, 'mid'), false); c.lineWidth = 3; }   // brows: one stroke along the middle (filled shapes thin badly when traced)
    for (const [a, b] of [[33, 42], [87, 96]]) {                                                  // eyes: lid outline, and an iris and pupil clipped by the lids
      const eye = byAngle(P(L, range(a, b))), [ex, ey] = centre(eye), ew = Math.max(...eye.map((p) => p[0])) - Math.min(...eye.map((p) => p[0])), eh = Math.max(...eye.map((p) => p[1])) - Math.min(...eye.map((p) => p[1]));
      c.save();
      c.beginPath(); eye.forEach((p, i) => (i ? c.lineTo(p[0], p[1]) : c.moveTo(p[0], p[1]))); c.closePath(); c.clip();
      const ir = Math.max(ew * 0.18, eh * 0.5);
      c.lineWidth = 2; c.beginPath(); c.arc(ex, ey, ir, 0, Math.PI * 2); c.stroke();
      c.restore();
      c.lineWidth = 3.5; c.beginPath(); eye.forEach((p, i) => (i ? c.lineTo(p[0], p[1]) : c.moveTo(p[0], p[1]))); c.closePath(); c.stroke(); c.lineWidth = 3;
    }
    curve(c, P(L, [76, 77, 78, 79, 80, 85, 84, 83, 82]), false);                                  // nose, with nostrils
    for (const i of [79, 84]) { c.beginPath(); c.arc(X(L[i][0]), Y(L[i][1]), 2.5, 0, Math.PI * 2); c.fill(); }
    const mouth = P(L, range(52, 71)), cl = mouth.reduce((p, q) => (q[0] < p[0] ? q : p)), cr = mouth.reduce((p, q) => (q[0] > p[0] ? q : p)), an = Math.atan2(cr[1] - cl[1], cr[0] - cl[0]), [mx, my] = centre(mouth);
    const rot = (pts, a) => pts.map(([x, y]) => [mx + (x - mx) * Math.cos(a) - (y - my) * Math.sin(a), my + (x - mx) * Math.sin(a) + (y - my) * Math.cos(a)]);
    const flat = rot(mouth, -an), top = rot(bins(flat, 6, 'min'), an), bot = rot(bins(flat, 6, 'max'), an), mid = rot(bins(flat, 6, 'mid'), an);   // level the mouth first, so a tilted head still bins correctly
    curve(c, top.concat(bot.slice().reverse()), true);                                            // lips: top edge and bottom edge, joined at the corners
    curve(c, mid, false);                                                                          // the line where they part
  }
  return { cv, faces: boxes.length };
}
module.exports = { detect, landmarks, faceArt };
