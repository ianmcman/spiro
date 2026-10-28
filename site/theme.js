'use strict';
(function (root, factory) {
  if (typeof module === 'object' && module.exports) {
    module.exports = factory();
  } else {
    root.SpiroTheme = factory();
  }
})(typeof globalThis !== 'undefined' ? globalThis : this, function () {
  const THEMES = [
    {
      id: 'monokai',
      label: 'Monokai',
      scheme: 'dark',
      canvas: '#282923',
      meta: '#282923',
      vars: {
        '--bg': 'hsl(70, 8%, 15%)',
        '--card': 'hsl(70, 8%, 16%)',
        '--card-border': 'hsl(70, 8%, 19.4%)',
        '--card-shadow': 'hsl(70, 8%, 14.5%)',
        '--separator': 'hsl(70, 8%, 19.4%)',
        '--field-border': 'hsl(70, 8%, 27%)',
        '--track': 'hsl(70, 8%, 26%)',
        '--text': 'hsl(70, 8%, 63.8%)',
        '--text-hi': 'hsl(70, 8%, 93.5%)',
        '--text-sub': 'hsl(70, 8%, 38.5%)',
        '--positive': 'hsl(80, 76%, 53%)',
      },
    },
    {
      id: 'midnight',
      label: 'Midnight',
      scheme: 'dark',
      canvas: '#10141d',
      meta: '#10141d',
      vars: {
        '--bg': 'hsl(220, 24%, 10%)',
        '--card': 'hsl(220, 22%, 13%)',
        '--card-border': 'hsl(220, 20%, 18%)',
        '--card-shadow': 'hsl(220, 25%, 8%)',
        '--separator': 'hsl(220, 20%, 18%)',
        '--field-border': 'hsl(220, 18%, 25%)',
        '--track': 'hsl(220, 18%, 24%)',
        '--text': 'hsl(220, 15%, 70%)',
        '--text-hi': 'hsl(220, 20%, 94%)',
        '--text-sub': 'hsl(220, 12%, 45%)',
        '--positive': 'hsl(145, 65%, 50%)',
      },
    },
    {
      id: 'highcontrast',
      label: 'High contrast',
      scheme: 'dark',
      canvas: '#000000',
      meta: '#000000',
      vars: {
        '--bg': 'hsl(0, 0%, 0%)',
        '--card': 'hsl(0, 0%, 7%)',
        '--card-border': 'hsl(0, 0%, 30%)',
        '--card-shadow': 'hsl(0, 0%, 0%)',
        '--separator': 'hsl(0, 0%, 30%)',
        '--field-border': 'hsl(0, 0%, 45%)',
        '--track': 'hsl(0, 0%, 35%)',
        '--text': 'hsl(0, 0%, 88%)',
        '--text-hi': 'hsl(0, 0%, 100%)',
        '--text-sub': 'hsl(0, 0%, 65%)',
        '--positive': 'hsl(120, 100%, 50%)',
      },
    },
    {
      id: 'light',
      label: 'Light',
      scheme: 'light',
      canvas: '#ffffff',
      meta: '#f5f5f5',
      vars: {
        '--bg': 'hsl(0, 0%, 96%)',
        '--card': 'hsl(0, 0%, 100%)',
        '--card-border': 'hsl(0, 0%, 85%)',
        '--card-shadow': 'hsl(0, 0%, 88%)',
        '--separator': 'hsl(0, 0%, 85%)',
        '--field-border': 'hsl(0, 0%, 75%)',
        '--track': 'hsl(0, 0%, 80%)',
        '--text': 'hsl(0, 0%, 25%)',
        '--text-hi': 'hsl(0, 0%, 10%)',
        '--text-sub': 'hsl(0, 0%, 45%)',
        '--positive': 'hsl(130, 60%, 35%)',
      },
    },
  ];

  const ACCENTS = [
    { id: 'cyan', label: 'Cyan', color: '#66d9ef', dark: 'hsl(190, 81%, 67%)', light: 'hsl(190, 95%, 30%)' },
    { id: 'pink', label: 'Pink', color: '#f92672', dark: 'hsl(340, 82%, 60%)', light: 'hsl(340, 85%, 42%)' },
    { id: 'orange', label: 'Orange', color: '#fd971f', dark: 'hsl(32, 98%, 56%)', light: 'hsl(24, 95%, 40%)' },
    { id: 'yellow', label: 'Yellow', color: '#e6db74', dark: 'hsl(54, 90%, 60%)', light: 'hsl(40, 95%, 30%)' },
    { id: 'green', label: 'Green', color: '#a6e22e', dark: 'hsl(90, 70%, 55%)', light: 'hsl(120, 75%, 30%)' },
    { id: 'purple', label: 'Purple', color: '#ae81ff', dark: 'hsl(265, 85%, 70%)', light: 'hsl(270, 75%, 45%)' },
  ];

  function rgb(c) {
    let m = /^#([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i.exec(c);
    if (m) return [1, 2, 3].map((i) => parseInt(m[i], 16));
    m = /^hsl\(\s*([\d.]+)\s*,\s*([\d.]+)%\s*,\s*([\d.]+)%\s*\)$/i.exec(c);
    if (!m) return [0, 0, 0];
    const h = +m[1] / 360, s = +m[2] / 100, l = +m[3] / 100;
    const q = l < 0.5 ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
    const f = (t) => { t = (t + 1) % 1; return t < 1 / 6 ? p + (q - p) * 6 * t : t < 0.5 ? q : t < 2 / 3 ? p + (q - p) * (2 / 3 - t) * 6 : p; };
    return [f(h + 1 / 3), f(h), f(h - 1 / 3)].map((x) => Math.round(x * 255));
  }

  function lum(c) {
    const [r, g, b] = (Array.isArray(c) ? c : rgb(c)).map((v) => { v /= 255; return v <= 0.03928 ? v / 12.92 : ((v + 0.055) / 1.055) ** 2.4; });
    return 0.2126 * r + 0.7152 * g + 0.0722 * b;
  }

  function contrast(a, b) {
    const [x, y] = [lum(a), lum(b)].sort((p, q) => q - p);
    return (x + 0.05) / (y + 0.05);
  }

  function toHex(rgbArr) {
    return '#' + rgbArr.map((x) => Math.max(0, Math.min(255, Math.round(x))).toString(16).padStart(2, '0')).join('');
  }

  function findTheme(themeId) {
    return THEMES.find((t) => t.id === themeId) || THEMES[0];
  }

  function findAccent(accentId) {
    return ACCENTS.find((a) => a.id === accentId) || ACCENTS[0];
  }

  function themeVars(themeId, accentId) {
    const theme = findTheme(themeId);
    const accent = findAccent(accentId);
    const primary = theme.scheme === 'light' ? accent.light : accent.dark;
    return Object.assign({}, theme.vars, { '--primary': primary });
  }

  function canvasBackground(themeId) {
    return findTheme(themeId).canvas;
  }

  function metaColor(themeId) {
    const theme = findTheme(themeId);
    return theme.meta || theme.canvas;
  }

  function resolveThemeId(pref, prefersDark) {
    if (pref === 'system') return prefersDark ? 'monokai' : 'light';
    if (THEMES.some((t) => t.id === pref)) return pref;
    return 'monokai';
  }

  function inkFor(themeId, color) {
    const canvas = canvasBackground(themeId);
    if (contrast(color, canvas) >= 3) return color;
    const [r, g, b] = rgb(color);
    const cLum = lum(canvas);
    if (cLum > 0.5) {
      for (let f = 0.95; f > 0.05; f -= 0.02) {
        const candidate = toHex([r * f, g * f, b * f]);
        if (contrast(candidate, canvas) >= 3) return candidate;
      }
    } else {
      for (let f = 0.05; f < 1; f += 0.02) {
        const candidate = toHex([r + (255 - r) * f, g + (255 - g) * f, b + (255 - b) * f]);
        if (contrast(candidate, canvas) >= 3) return candidate;
      }
    }
    return color;
  }

  function loadPrefs(storage) {
    const defaults = { theme: 'monokai', accent: 'cyan' };
    if (!storage) return defaults;
    try {
      const raw = storage.getItem('spiro-theme');
      if (!raw) return defaults;
      const parsed = JSON.parse(raw);
      if (!parsed || typeof parsed !== 'object') return defaults;
      const themeValid = parsed.theme === 'system' || THEMES.some((t) => t.id === parsed.theme);
      const accentValid = ACCENTS.some((a) => a.id === parsed.accent);
      return {
        theme: themeValid ? parsed.theme : defaults.theme,
        accent: accentValid ? parsed.accent : defaults.accent,
      };
    } catch (_) {
      return defaults;
    }
  }

  function savePrefs(storage, prefs) {
    if (!storage) return false;
    try {
      const toSave = {
        theme: prefs && (prefs.theme === 'system' || THEMES.some((t) => t.id === prefs.theme)) ? prefs.theme : 'monokai',
        accent: prefs && ACCENTS.some((a) => a.id === prefs.accent) ? prefs.accent : 'cyan',
      };
      storage.setItem('spiro-theme', JSON.stringify(toSave));
      return true;
    } catch (_) {
      return false;
    }
  }

  return {
    THEMES,
    ACCENTS,
    themeVars,
    canvasBackground,
    metaColor,
    resolveThemeId,
    inkFor,
    loadPrefs,
    savePrefs,
  };
});
