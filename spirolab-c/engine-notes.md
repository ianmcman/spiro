# Spiro Engine Implementation Notes

## Architecture & Math
- **Shapes**: All 11 generators (`circle`, `ellipse`, `squircle`, `heart`, `rosette`, `triangle`, `square`, `pentagon`, `hexagon`, `star`, `burst`) ported from reference math.
- **Normalization**: Polygon/curve centroid computed via signed area integrals; coordinates centered and normalized to unit radius $R = 1.0$ with CCW orientation.
- **Gears & Closure**: Tooth count ratio determines wheel radius with inside wheel capped at $0.97$. Closure search over $k \in [1, 400]$ ensures closed curves.
- **Classic Analytic Mode**: Circle/circle combinations use exact analytic hypotrochoid/epitrochoid equations for machine-precision accuracy.
- **Polyline & Sharp-Corner Handling**: Full fillet rounding for smooth wheels rolling on polygon rings, including corner detection, ponytail gap clipping, arc insertion, and accelerated fillet segments (`FAST_WEIGHT = 0.03`). Sharp-corner pivots track pending rotation events (`next_sample`, `ahead`, `part_rot`) without departures from reference geometry.
- **Second Wheel**: Epicyclic secondary wheel dynamics ported for both analytic and polyline rolling frames.
- **Safety**: Fully reentrant, heap-allocated path ownership, zero mutable globals, strict bounds and finiteness checks, null-safe cleanup.
