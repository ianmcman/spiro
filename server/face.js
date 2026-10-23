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
// Points along a smooth curve through pts (quadratics between midpoints), about `step` px apart.
function sample(pts, closed, step = 1.2) {
  const out = [], mid = (p, q) => [(p[0] + q[0]) / 2, (p[1] + q[1]) / 2], n = pts.length;
  if (n < 2) return out;
  const quad = (a, ctl, e) => {
    const len = Math.hypot(ctl[0] - a[0], ctl[1] - a[1]) + Math.hypot(e[0] - ctl[0], e[1] - ctl[1]), m = Math.max(2, Math.ceil(len / step));
    for (let i = 0; i < m; i++) { const t = i / m, u = 1 - t; out.push([u * u * a[0] + 2 * u * t * ctl[0] + t * t * e[0], u * u * a[1] + 2 * u * t * ctl[1] + t * t * e[1]]); }
  };
  if (closed) {
    let cur = mid(pts[n - 1], pts[0]);
    for (let i = 0; i < n; i++) { const e = mid(pts[i], pts[(i + 1) % n]); quad(cur, pts[i], e); cur = e; }
    out.push(out[0]);
  } else {
    let cur = pts[0];
    for (let i = 1; i < n - 1; i++) { const e = mid(pts[i], pts[i + 1]); quad(cur, pts[i], e); cur = e; }
    quad(cur, mid(cur, pts[n - 1]), pts[n - 1]);
    out.push(pts[n - 1]);
  }
  return out;
}
const circle = (x, y, r, a0 = 0, a1 = Math.PI * 2) => Array.from({ length: Math.max(8, Math.ceil(r * (a1 - a0) / 1.2)) + 1 }, (_, i) => [x + r * Math.cos(a0 + (a1 - a0) * i / Math.max(8, Math.ceil(r * (a1 - a0) / 1.2))), y + r * Math.sin(a0 + (a1 - a0) * i / Math.max(8, Math.ceil(r * (a1 - a0) / 1.2)))]);
function inPoly(p, poly) {
  let ins = false;
  for (let i = 0, j = poly.length - 1; i < poly.length; j = i++) if ((poly[i][1] > p[1]) !== (poly[j][1] > p[1]) && p[0] < (poly[j][0] - poly[i][0]) * (p[1] - poly[i][1]) / (poly[j][1] - poly[i][1]) + poly[i][0]) ins = !ins;
  return ins;
}
// Points binned along x: per bin the middle, topmost or bottommost point.
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

/* ---------------------------------------------------------------- the drawing, as one path */
// Returns the faces as a path in a crop of the picture (long side 640): { ox, oy, faces }. The features are exact curves.
// The subject's outline and the photo's edges (sketch) are walked as thin raster lines, then everything is joined; pen lifts
// are the gaps between pieces. mask (optional): 1 = subject, drawn as an outline.
async function faceVectors(src, mask, sketch = false) {
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
  const k = 640 / Math.max(rw, rh), cw = Math.round(rw * k), ch = Math.round(rh * k), N = cw * ch, X = (x) => (x - rx0) * k, Y = (y) => (y - ry0) * k;
  const P = (L, ids) => ids.map((i) => [X(L[i][0]), Y(L[i][1])]);
  const skin = boxes.map((b) => ({ x0: X(b.x1), y0: Y(b.y1), x1: X(b.x2), y1: Y(b.y2) }));
  const zoneAround = (e, mx, my) => { const ex = e.map((p) => p[0]), ey = e.map((p) => p[1]), w = Math.max(...ex) - Math.min(...ex); return { x0: Math.min(...ex) - w * mx, x1: Math.max(...ex) + w * mx, y0: Math.min(...ey) - w * my, y1: Math.max(...ey) + w * my }; };
  const eyeZone = all.flatMap((L) => [[33, 42], [87, 96]].map(([a, b]) => zoneAround(P(L, range(a, b)), 1.0, 1.0)));
  const own = all.flatMap((L) => [[33, 42], [87, 96], [43, 51], [97, 105]].map(([a, b]) => zoneAround(P(L, range(a, b)), 0.04, 0.04)));
  const within = (z, x, y) => x >= z.x0 && x <= z.x1 && y >= z.y0 && y <= z.y1;
  const inside = new Uint8Array(N);
  for (let y = 0; y < ch; y++) for (let x = 0; x < cw; x++) {
    const sx = Math.min(W - 1, Math.max(0, Math.round(rx0 + x / k))), sy = Math.min(H - 1, Math.max(0, Math.round(ry0 + y / k)));
    if (mask) inside[y * cw + x] = mask[sy * W + sx] > 0.5 ? 1 : 0;
    else inside[y * cw + x] = skin.some((z) => within({ x0: z.x0 - (z.x1 - z.x0) * 0.4, x1: z.x1 + (z.x1 - z.x0) * 0.4, y0: z.y0 - (z.y1 - z.y0) * 0.4, y1: z.y1 + (z.y1 - z.y0) * 0.4 }, x, y)) ? 1 : 0;
  }
  const b = new Uint8Array(N);                                      // thin raster lines: the outline and the photo's edges
  if (mask) {
    const sm = new Float32Array(N);
    for (let pass = 0; pass < 2; pass++) {
      const from = pass ? sm.slice() : Float32Array.from(inside);
      for (let y = 2; y < ch - 2; y++) for (let x = 2; x < cw - 2; x++) {
        let t = 0;
        for (let dy = -2; dy <= 2; dy++) for (let dx = -2; dx <= 2; dx++) t += from[(y + dy) * cw + x + dx];
        sm[y * cw + x] = t / 25;
      }
    }
    for (let y = 3; y < ch - 3; y++) for (let x = 3; x < cw - 3; x++) {
      const i = y * cw + x;
      if (sm[i] > 0.5 && (sm[i - 1] <= 0.5 || sm[i + 1] <= 0.5 || sm[i - cw] <= 0.5 || sm[i + cw] <= 0.5)) b[i] = 1;
    }
  }
  if (sketch) {                                                     // Canny edges, off the clean skin and away from the drawn eyes and brows
    const g = E.createCanvas(cw, ch), gx = g.getContext('2d', { willReadFrequently: true });
    gx.filter = 'grayscale(1) blur(1.4px)';
    gx.fillStyle = '#fff'; gx.fillRect(0, 0, cw, ch);
    gx.drawImage(src, rx0, ry0, rw, rh, 0, 0, cw, ch);
    const d = gx.getImageData(0, 0, cw, ch).data, mag = new Float32Array(N), dir = new Uint8Array(N), keep = new Uint8Array(N), zone = new Uint8Array(N);
    for (let y = 3; y < ch - 3; y++) for (let x = 3; x < cw - 3; x++) {
      const i = y * cw + x, v = (j) => d[4 * j];
      const sx = v(i - cw + 1) + 2 * v(i + 1) + v(i + cw + 1) - v(i - cw - 1) - 2 * v(i - 1) - v(i + cw - 1), sy = v(i + cw - 1) + 2 * v(i + cw) + v(i + cw + 1) - v(i - cw - 1) - 2 * v(i - cw) - v(i - cw + 1);
      mag[i] = Math.hypot(sx, sy);
      dir[i] = Math.round((Math.atan2(sy, sx) + Math.PI) / Math.PI * 4) % 4;
      const inEye = eyeZone.some((z) => within(z, x, y));
      if (inside[i] && !own.some((z) => within(z, x, y)) && (inEye || !skin.some((z) => within(z, x, y)))) keep[i] = 1;
      zone[i] = inEye ? 1 : 0;
    }
    const step = [[1, 0], [1, 1], [0, 1], [-1, 1]], thin = new Float32Array(N);
    for (let y = 3; y < ch - 3; y++) for (let x = 3; x < cw - 3; x++) {
      const i = y * cw + x, [dx, dy] = step[dir[i]];
      if (keep[i] && mag[i] >= mag[i + dy * cw + dx] && mag[i] >= mag[i - dy * cw - dx]) thin[i] = mag[i];
    }
    const hi = (zn) => { const v = []; for (let i = 0; i < N; i += 2) if (keep[i] && zone[i] === zn && thin[i] > 0) v.push(thin[i]); v.sort((p, q) => p - q); return v.length ? v[Math.floor(v.length * (zn ? 0.78 : 0.96))] : 1e9; };
    const hiT = [hi(0), hi(1)], mark = new Uint8Array(N);
    for (let s0 = 0; s0 < N; s0++) {
      if (!(thin[s0] > hiT[zone[s0]]) || mark[s0]) continue;
      const list = [s0]; mark[s0] = 1;
      for (let h = 0; h < list.length; h++) for (let dy = -1; dy <= 1; dy++) for (let dx = -1; dx <= 1; dx++) {
        const q = list[h] + dy * cw + dx;
        if (q >= 0 && q < N && !mark[q] && thin[q] > hiT[zone[q]] * 0.5) { mark[q] = 1; list.push(q); }
      }
      if (list.length >= 40) for (const i of list) b[i] = 1;          // short scraps are noise
    }
  }
  const fat = new Uint8Array(N);                                   // 3 px wide lines thin to a clean centreline; 1 px lines break up
  for (let y = 2; y < ch - 2; y++) for (let x = 2; x < cw - 2; x++) if (b[y * cw + x]) for (let dy = -1; dy <= 1; dy++) for (let dx = -1; dx <= 1; dx++) fat[(y + dy) * cw + x + dx] = 1;
  const walk = E.maskTerms(fat, cw, ch, { n: 1, pitch: ch, size: 0, len: 80, dots: false, minBlob: 12, pathOnly: true });
  const ox = walk ? Array.from(walk.ox) : [], oy = walk ? Array.from(walk.oy) : [];
  const add = (poly) => { for (const [x, y] of poly) { ox.push(x); oy.push(y); } };
  for (const L of all) {
    if (!mask) add(sample(P(L, JAW), false));                                                      // no subject outline: the jaw from the landmarks
    for (const [a, c] of [[43, 51], [97, 105]]) add(sample(bins(P(L, range(a, c)), 6, 'mid'), false));          // brows: one stroke along the middle
    for (const [a, c] of [[33, 42], [87, 96]]) {                                                    // eyes: the lid outline, and an iris cut off by the lids
      const eye = byAngle(P(L, range(a, c))), [ex, ey] = centre(eye), ew = Math.max(...eye.map((p) => p[0])) - Math.min(...eye.map((p) => p[0])), eh = Math.max(...eye.map((p) => p[1])) - Math.min(...eye.map((p) => p[1]));
      add(sample(eye, true));
      const ring = circle(ex, ey, Math.max(ew * 0.18, eh * 0.5));
      let arc = [];
      for (const p of ring) { if (inPoly(p, eye)) arc.push(p); else { if (arc.length > 2) add(arc); arc = []; } }
      if (arc.length > 2) add(arc);
    }
    add(sample(P(L, [76, 77, 78, 79, 80, 85, 84, 83, 82]), false));                                // nose, with nostrils
    for (const i of [79, 84]) add(circle(X(L[i][0]), Y(L[i][1]), 2.2));
    const mouth = P(L, range(52, 71)), cl = mouth.reduce((p, q) => (q[0] < p[0] ? q : p)), cr = mouth.reduce((p, q) => (q[0] > p[0] ? q : p)), an = Math.atan2(cr[1] - cl[1], cr[0] - cl[0]), [mx, my] = centre(mouth);
    const rot = (pts, t) => pts.map(([x, y]) => [mx + (x - mx) * Math.cos(t) - (y - my) * Math.sin(t), my + (x - mx) * Math.sin(t) + (y - my) * Math.cos(t)]);
    const flat = rot(mouth, -an), top = rot(bins(flat, 6, 'min'), an), bot = rot(bins(flat, 6, 'max'), an), mid = rot(bins(flat, 6, 'mid'), an);   // level the mouth first
    add(sample(top.concat(bot.slice().reverse()), true));                                           // lips: top edge and bottom edge, joined at the corners
    add(sample(mid, false));                                                                         // the line where they part
  }
  return ox.length ? { ox, oy, faces: boxes.length } : null;
}
module.exports = { detect, landmarks, faceVectors };
