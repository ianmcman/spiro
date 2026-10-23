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
// Black lines on white, cropped around the faces (long side 640). mask (optional): subject mask over src, drawn as an outline.
async function faceArt(src, mask) {
  let boxes = await detect(src);
  if (!boxes.length) return null;
  boxes = boxes.slice(0, 6);
  const all = [];
  for (const b of boxes) all.push(await landmarks(src, b));
  let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity;
  for (const b of boxes) { x0 = Math.min(x0, b.x1); y0 = Math.min(y0, b.y1); x1 = Math.max(x1, b.x2); y1 = Math.max(y1, b.y2); }
  const fw = x1 - x0, fh = y1 - y0, m = mask ? 0.9 : 0.25;                // margin around the faces (more with the subject outline: hair, shoulders)
  const rx0 = x0 - fw * m, ry0 = y0 - fh * m * (mask ? 0.8 : 1), rw = fw * (1 + 2 * m), rh = fh * (1 + (mask ? 1.9 : 2) * m);
  const k = 640 / Math.max(rw, rh), cw = Math.round(rw * k), ch = Math.round(rh * k), X = (x) => (x - rx0) * k, Y = (y) => (y - ry0) * k, cv = E.createCanvas(cw, ch), c = cv.getContext('2d');
  c.fillStyle = '#fff'; c.fillRect(0, 0, cw, ch);
  c.strokeStyle = '#000'; c.lineWidth = 3; c.lineJoin = c.lineCap = 'round';
  if (mask) {                                                       // the subject's outline: boundary pixels of the mask
    const W = src.width, H = src.height;
    c.fillStyle = '#000';
    for (let y = 1; y < H - 1; y += 1) for (let x = 1; x < W - 1; x += 1) {
      const i = y * W + x;
      if (mask[i] > 0.5 && (mask[i - 1] <= 0.5 || mask[i + 1] <= 0.5 || mask[i - W] <= 0.5 || mask[i + W] <= 0.5)) {
        const px = X(x), py = Y(y);
        if (px >= 0 && py >= 0 && px < cw && py < ch) c.fillRect(px - 1.2, py - 1.2, 2.4, 2.4);
      }
    }
  }
  for (const L of all) {
    const P = (ids) => ids.map((i) => [X(L[i][0]), Y(L[i][1])]);
    curve(c, P(JAW), false);
    for (const [a, b] of [[43, 51], [97, 105]]) curve(c, byAngle(P(range(a, b))), true);          // brows
    for (const [a, b] of [[33, 42], [87, 96]]) {                                                  // eyes, with an iris
      const eye = P(range(a, b)), [ex, ey] = centre(eye), w2 = Math.max(...eye.map((p) => p[0])) - Math.min(...eye.map((p) => p[0]));
      curve(c, byAngle(eye), true);
      c.beginPath(); c.arc(ex, ey, w2 * 0.16, 0, Math.PI * 2); c.stroke();
    }
    curve(c, P([76, 77, 78, 79, 80, 85, 84, 83, 82]), false);                                     // nose
    curve(c, P([73, 74]), false);
    const mouth = P(range(52, 71)), outer = hull(mouth), inner = mouth.filter((p) => !outer.includes(p));
    curve(c, outer, true);
    if (inner.length > 3) curve(c, byAngle(inner), true);
  }
  return { cv, faces: boxes.length };
}
module.exports = { detect, landmarks, faceArt };
