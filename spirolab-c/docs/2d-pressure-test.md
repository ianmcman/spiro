# Native 2D limits and renderer comparison — 2026-11-11

The C app now supports a second round wheel rolling inside or outside the primary wheel. Its geometry, polygon-ring rolling, circle overlays, saved settings and GUI controls were tested. Existing ten-value parameter records load as inside-wheel designs; new records preserve the side of both control and drawn parameters.

The 2D limits are 4,194,305 curve points, 2,097,152 Fourier circles and 8192-pixel trace detail. They are powers of two for FFT processing, with a repeated endpoint for closure. Gear sampling adapts to complexity up to that ceiling; ordinary curves do not allocate the maximum unnecessarily. Defaults remain 256 circles and 384-pixel detail. The document limit is 512 MiB and 8,388,610 total points, allowing a maximum-size current curve plus one maximum-size kept layer. AI preparation reduces images to 4096 pixels while the source remains available for ordinary tracing.

These are tested practical ceilings on the i7-12700F/46 GiB PC, rather than an assertion that every possible input will finish promptly or that higher allocations would fail. Dense/noisy images and many layers can be much more expensive. Raising them further requires another performance and persistence audit.

## CPU preparation and Cairo rendering

Optimized C build, 1000×800 Cairo image surface, a two-frequency closed planar curve. All forward/inverse FFTs and an 8192-pixel text trace with the maximum circle count succeeded. Sparse-mask timings are not representative of dense/noisy pictures.

| Fourier circles | Reconstructed points | Forward FFT | Inverse FFT | Full solid Cairo redraw |
|---:|---:|---:|---:|---:|
| 65,536 | 131,073 | 18.8 ms | 5.3 ms | 69.0 ms |
| 131,072 | 262,145 | 40.3 ms | 11.4 ms | 119.1 ms |
| 262,144 | 524,289 | 83.9 ms | 24.6 ms | 219.1 ms |
| 524,288 | 1,048,577 | 173.9 ms | 54.1 ms | 384.4 ms |
| 1,048,576 | 2,097,153 | 365.7 ms | 139.8 ms | 732.5 ms |
| 2,097,152 | 4,194,305 | 794.0 ms | 298.9 ms | 1557.3 ms |

The maximum text trace took about 3.9 seconds. Before preview optimization the process peaked around 1.2 GiB RSS, mostly while stroking millions of points in Cairo. The screen preview now skips points within 0.2 screen pixels of the last emitted point, preserving pen lifts and endpoints, and limits high-count rainbow colour groups. At maximum resolution, the same synthetic curve then took 128 ms for solid ink and 79 ms for rainbow, with approximately 273 MiB peak RSS. Zooming increases the number of retained preview segments. Exports keep all points.

The dedicated high-resolution check also generated the maximum text trace, saved every point, Fourier coefficient and pen-lift flag, reopened it and verified the restored settings and sampled geometry. See `tests/high-resolution.sh`.

## OpenGL alternative

A prototype passes the identical reconstructed planar points to the existing 3D hardware line renderer, with all z coordinates zero and camera tilt zero. Projection is scaled to match the Cairo viewport. This uses the RTX 4070 SUPER, retained vertex buffers, depth/MSAA, one closed line and solid colour. Forty completed frames with small pan changes are timed after upload using `glFinish()`.

| Points | Optimized Cairo preview | Hardware OpenGL | Initial GPU upload/draw |
|---:|---:|---:|---:|
| 131,073 | 63.64 ms | 0.068 ms | 2.52 ms |
| 262,145 | 110.82 ms | 0.093 ms | 2.88 ms |
| 524,289 | 105.39 ms | 0.141 ms | 6.49 ms |
| 1,048,577 | 104.91 ms | 0.238 ms | 14.84 ms |
| 2,097,153 | 111.17 ms | 0.445 ms | 29.51 ms |
| 4,194,305 | 111.42 ms | 0.862 ms | 59.94 ms |

This comparison uses a 758×762 viewport and excludes GTK input latency/compositor presentation. It demonstrates substantial GPU headroom, but is not a completed 2D GPU implementation: disconnected strokes, kept layers, opacity and stroke-width matching still need adaptation and validation. The production 2D viewport remains Cairo. OpenGL is the preferred next step for interactive drawing; Cairo continues to serve vector/PDF exports and fallback rendering.

Raw timings: [2d-pressure-results.txt](2d-pressure-results.txt). Benchmark sources: `tests/2d-pressure.c` and `tests/flat-gpu-pressure.c`. The former accepts FFT size and detail as arguments; the latter requires a hardware OpenGL desktop session and runs a fixed increasing-size comparison.


```sh
./tests/pressure2d.sh                 # Maximum FFT/detail and Cairo preparation benchmark
./tests/pressure2d.sh --gpu           # Planar hardware OpenGL comparison; requires a display
./tests/high-resolution.sh            # Maximum-data saved-document roundtrip
```
