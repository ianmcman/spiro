// How music bends the 3D spirograph. Shared by the 3D page (live) and the lab server (video export), so both look the same.
// step(raw, dt, nowMs, arms, A): raw = [bass, mid, high] band levels 0..1; returns { arms, hueShift, rotSpeed }.
(function (root, make) { if (typeof module === 'object' && module.exports) module.exports = make(); else root.Reactive = make(); })(this, function () {
  function create() {
    const st = { peak: [0.05, 0.05, 0.05], lev: [0, 0, 0], hist: [], kick: 0, lastBeat: 0, drift: [], hueShift: 0 };
    return {
      step(raw, dt, now, arms, A) {
        raw.forEach((r, j) => {
          st.peak[j] = Math.max(r, st.peak[j] * 0.998);                       // each band against its own recent loudest, so quiet sources still move
          const l = st.peak[j] > 0.04 ? Math.pow(Math.min(1, r / st.peak[j]), 3 / A.sens) : 0;
          st.lev[j] = Math.max(l, st.lev[j] * Math.pow(0.001, dt));            // fast attack, release in about a third of a second
        });
        st.hist.push(raw[0]); if (st.hist.length > 43) st.hist.shift();
        const avg = st.hist.reduce((t, v) => t + v, 0) / st.hist.length;
        if (raw[0] > avg * 1.35 && raw[0] > 0.12 && now - st.lastBeat > 220) { st.kick = 1; st.lastBeat = now; }
        st.kick *= Math.pow(0.02, dt);
        const m = A.motion, lev = st.lev;
        const live = arms.map((a, k) => {
          const j = Math.min(k, 2), sg = k % 2 ? 1 : -1;
          st.drift[k] = (st.drift[k] || 0) + dt * m * (6 + 80 * lev[j]) * sg;
          return { L: a.L * (1 + m * 0.7 * lev[j]), w: a.w, tilt: a.tilt + m * 35 * (lev[1] - 0.3) * sg, az: a.az + st.drift[k] * 0.5, ph: a.ph + st.drift[k] + m * 70 * st.kick * sg * Math.sqrt(k + 1) };
        });
        st.hueShift += dt * m * (8 + 160 * lev[2]);
        return { arms: live, hueShift: st.hueShift, rotSpeed: 1.5 + 6 * m * lev[1] };
      },
    };
  }
  return { create };
});
