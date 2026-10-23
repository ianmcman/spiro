// Prints checksums of a few models built by the server engine; compare with the same configs in the browser (see below).
const E = require('./engine');
const cfgs = [
  {}, { ringShape: 'hexagon', wheelShape: 'star', side: 'outside', pen: 120 },
  { second: true, w2side: 'inside', w2div: 3, w2speed: 2, wheelTeeth: 45, ringTeeth: 105, sizing: 'teeth', pen: 110 },
  { text: 'Ian' }, { text: 'The quick brown fox jumps over the lazy dog' }
];
const sums = cfgs.map((c) => {
  const p = Object.assign({}, E.DEFAULTS, c), m = E.buildModel(p, 390, 844);
  E.fill(m, m.N);
  let a = 0;
  for (let i = 0; i <= m.N; i += 7) a += m.xs[i] + 2 * m.ys[i];
  return m.N + ':' + a.toFixed(2);
});
console.log(JSON.stringify(sums));
