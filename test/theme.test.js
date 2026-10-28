'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const T = require('../site/theme.js');

const SITE = path.join(__dirname, '..', 'site');
const CSS_VARS = ['--bg', '--card', '--card-border', '--card-shadow', '--separator', '--field-border', '--track', '--text', '--text-hi', '--text-sub', '--primary', '--positive'];
const INKS = ['#f92672', '#fd971f', '#e6db74', '#a6e22e', '#66d9ef', '#ae81ff', '#ffffff'];

function rgb(c) {
  let m = /^#([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i.exec(c);
  if (m) return [1, 2, 3].map((i) => parseInt(m[i], 16));
  m = /^hsl\(\s*([\d.]+)\s*,\s*([\d.]+)%\s*,\s*([\d.]+)%\s*\)$/i.exec(c);
  assert.ok(m, `unsupported colour format: ${c}`);
  const h = +m[1] / 360, s = +m[2] / 100, l = +m[3] / 100;
  const q = l < 0.5 ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
  const f = (t) => { t = (t + 1) % 1; return t < 1 / 6 ? p + (q - p) * 6 * t : t < 0.5 ? q : t < 2 / 3 ? p + (q - p) * (2 / 3 - t) * 6 : p; };
  return [f(h + 1 / 3), f(h), f(h - 1 / 3)].map((x) => Math.round(x * 255));
}
function lum(c) {
  const [r, g, b] = rgb(c).map((v) => { v /= 255; return v <= 0.03928 ? v / 12.92 : ((v + 0.055) / 1.055) ** 2.4; });
  return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}
const contrast = (a, b) => { const [x, y] = [lum(a), lum(b)].sort((p, q) => q - p); return (x + 0.05) / (y + 0.05); };
const mem = () => { const d = {}; return { getItem: (k) => (k in d ? d[k] : null), setItem: (k, v) => { d[k] = String(v); }, d }; };

test('theme list has the required themes, each with a complete variable set', () => {
  const ids = T.THEMES.map((t) => t.id);
  for (const id of ['monokai', 'midnight', 'highcontrast', 'light']) assert.ok(ids.includes(id), id);
  assert.equal(new Set(ids).size, ids.length);
  for (const t of T.THEMES) {
    assert.ok(t.label && ['dark', 'light'].includes(t.scheme), t.id);
    assert.match(t.canvas, /^#[0-9a-f]{6}$/i);
    for (const v of CSS_VARS.filter((v) => v !== '--primary')) assert.ok(t.vars[v], `${t.id} ${v}`);
  }
  assert.equal(T.THEMES.find((t) => t.id === 'light').scheme, 'light');
});

test('monokai with the default accent reproduces the current palette exactly', () => {
  const v = T.themeVars('monokai', 'cyan');
  assert.deepEqual(v, {
    '--bg': 'hsl(70, 8%, 15%)', '--card': 'hsl(70, 8%, 16%)', '--card-border': 'hsl(70, 8%, 19.4%)',
    '--card-shadow': 'hsl(70, 8%, 14.5%)', '--separator': 'hsl(70, 8%, 19.4%)', '--field-border': 'hsl(70, 8%, 27%)',
    '--track': 'hsl(70, 8%, 26%)', '--text': 'hsl(70, 8%, 63.8%)', '--text-hi': 'hsl(70, 8%, 93.5%)',
    '--text-sub': 'hsl(70, 8%, 38.5%)', '--primary': 'hsl(190, 81%, 67%)', '--positive': 'hsl(80, 76%, 53%)',
  });
  assert.equal(T.canvasBackground('monokai'), '#282923');
  assert.equal(T.metaColor('monokai'), '#282923');
});

test('every theme and accent is readable', () => {
  assert.ok(T.ACCENTS.length >= 6);
  for (const id of ['cyan', 'pink', 'orange', 'yellow', 'green', 'purple']) assert.ok(T.ACCENTS.some((a) => a.id === id), id);
  for (const t of T.THEMES) {
    for (const a of T.ACCENTS) {
      const v = T.themeVars(t.id, a.id);
      for (const k of CSS_VARS) assert.ok(v[k], `${t.id}/${a.id} ${k}`);
      assert.ok(contrast(v['--text'], v['--bg']) >= 4.5, `${t.id} text`);
      assert.ok(contrast(v['--text-hi'], v['--bg']) >= 7, `${t.id} text-hi`);
      assert.ok(contrast(v['--primary'], v['--card']) >= 3, `${t.id}/${a.id} primary`);
    }
    assert.match(T.metaColor(t.id), /^#[0-9a-f]{6}$/i);
  }
  assert.ok(contrast(T.themeVars('highcontrast', 'cyan')['--text'], T.themeVars('highcontrast', 'cyan')['--bg']) >= 12);
});

test('accents change only --primary', () => {
  const a = T.themeVars('midnight', 'pink'), b = T.themeVars('midnight', 'green');
  assert.notEqual(a['--primary'], b['--primary']);
  for (const k of CSS_VARS.filter((k) => k !== '--primary')) assert.equal(a[k], b[k], k);
});

test('unknown ids fall back to monokai and cyan', () => {
  assert.deepEqual(T.themeVars('nope', 'nope'), T.themeVars('monokai', 'cyan'));
  assert.deepEqual(T.themeVars(undefined, undefined), T.themeVars('monokai', 'cyan'));
  assert.equal(T.canvasBackground('nope'), '#282923');
});

test('resolveThemeId maps system to a theme by the OS preference', () => {
  assert.equal(T.resolveThemeId('system', true), 'monokai');
  assert.equal(T.resolveThemeId('system', false), 'light');
  assert.equal(T.resolveThemeId('midnight', false), 'midnight');
  assert.equal(T.resolveThemeId('bogus', true), 'monokai');
  assert.equal(T.resolveThemeId(undefined, false), 'monokai');
});

test('ink colours stay visible on every canvas and are left alone when already visible', () => {
  for (const t of T.THEMES) {
    for (const ink of INKS) {
      const out = T.inkFor(t.id, ink);
      assert.ok(contrast(out, t.canvas) >= 3, `${t.id} ${ink} -> ${out}`);
      if (contrast(ink, t.canvas) >= 3) assert.equal(out, ink, `${t.id} ${ink} should be unchanged`);
    }
  }
  for (const ink of INKS) assert.equal(T.inkFor('monokai', ink), ink);
});

test('prefs round-trip through storage and survive bad storage', () => {
  assert.deepEqual(T.loadPrefs(mem()), { theme: 'monokai', accent: 'cyan' });
  const s = mem();
  assert.equal(T.savePrefs(s, { theme: 'system', accent: 'purple' }), true);
  assert.ok(s.d['spiro-theme']);
  assert.deepEqual(T.loadPrefs(s), { theme: 'system', accent: 'purple' });
  const bad = mem(); bad.setItem('spiro-theme', '{not json');
  assert.deepEqual(T.loadPrefs(bad), { theme: 'monokai', accent: 'cyan' });
  const unk = mem(); unk.setItem('spiro-theme', JSON.stringify({ theme: 'x', accent: 'y' }));
  assert.deepEqual(T.loadPrefs(unk), { theme: 'monokai', accent: 'cyan' });
  const boom = { getItem() { throw new Error('denied'); }, setItem() { throw new Error('denied'); } };
  assert.deepEqual(T.loadPrefs(boom), { theme: 'monokai', accent: 'cyan' });
  assert.equal(T.savePrefs(boom, { theme: 'light', accent: 'cyan' }), false);
  assert.deepEqual(T.loadPrefs(null), { theme: 'monokai', accent: 'cyan' });
  assert.equal(T.savePrefs(null, { theme: 'light', accent: 'cyan' }), false);
});

test('the pages load the module and the service worker caches it', () => {
  for (const f of ['index.html', '3d.html']) assert.match(fs.readFileSync(path.join(SITE, f), 'utf8'), /<script[^>]+src="\/?theme\.js"/, f);
  assert.match(fs.readFileSync(path.join(SITE, 'sw.js'), 'utf8'), /theme\.js/);
});
