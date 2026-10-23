// The page's own engine, run in Node: site/index.html between the ENGINE-BEGIN and ENGINE-END markers is
// sliced out and evaluated with a canvas from @napi-rs/canvas, so the server builds exactly what the browser would.
const fs = require('fs');
const path = require('path');
const { createCanvas, GlobalFonts, Path2D } = require('@napi-rs/canvas');

const SITE = process.env.SITE_DIR || path.join(__dirname, '..', 'site');
GlobalFonts.registerFromPath(path.join(SITE, 'JetBrainsMono-Regular.woff2'), 'JetBrains Mono');

const html = fs.readFileSync(path.join(SITE, 'index.html'), 'utf8');
const a = html.indexOf('/*ENGINE-BEGIN'), b = html.indexOf('/*ENGINE-END*/');
if (a < 0 || b < a) throw new Error('ENGINE markers not found in index.html');
const body = html.slice(a, b);

// What the block expects from the page around it; none of it is used on the server paths.
const stubs = { document: { createElement: () => createCanvas(300, 150) }, Path2D, window: { devicePixelRatio: 1 },
  $: () => ({}), P: {}, zoom: { z: 1, x: 0, y: 0 }, vw: 0, vh: 0, dpr: 1, drawn: 0, model: null,
  view: null, vctx: null, tctx: null, bctx: null };
const exported = ['buildModel', 'fill', 'nameLines', 'nameTerms', 'picTerms', 'strokeRange', 'prepCtx', 'inkRGB', 'matching', 'picColor',
  'drawWheels', 'drawEpicycles', 'nameCache', 'INK_ALPHA', 'MAX_TEXT', 'nameN', 'fourierModel', 'SHAPES', 'INKS', 'maskTerms', 'pathTerms'];
const make = new Function(...Object.keys(stubs), body + '\nreturn {' + exported.join(',') + '};');
// Default settings: the page's own state literal, so the server can't disagree with it about names or ranges.
const lit = html.slice(html.indexOf('const P = {'), html.indexOf('};', html.indexOf('const P = {')) + 2);
const DEFAULTS = new Function(lit + '\nreturn P;')();
const list = (n) => new Function('return ' + html.match(new RegExp('const ' + n + ' = (\\[[^\\]]*\\])'))[1])();   // the page's tooth-count lists
module.exports = Object.assign(make(...Object.values(stubs)), { createCanvas, Path2D, DEFAULTS, RING_TEETH: list('RING_TEETH'), WHEEL_TEETH: list('WHEEL_TEETH') });
