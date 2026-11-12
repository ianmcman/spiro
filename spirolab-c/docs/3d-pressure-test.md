# 3D rendering pressure test — 2026-11-11

Tested the native OpenGL viewer on an RTX 4070 SUPER (12 GB), i7-12700F and 46 GiB RAM. Hardware acceleration was active. The normal application remains limited to 128,000 points, with 32,000 by default. Tests raised the bound only in disposable source copies.

## Results

Default three-arm design, twelve-sided tube, hardware multisample rendering, normal window size. Each size ran in a fresh process. Orbit timings include drawing and `glFinish()` for 40 changing camera views after the initial upload. They measure completed renderer frames, rather than input latency or presentation through the desktop compositor. Total VRAM includes the desktop and other applications.

| Points | Line median | Tube median | Tube p95 | Tube build + upload | Peak process RSS |
|---:|---:|---:|---:|---:|---:|
| 128,000 | 0.06 ms | 0.80 ms | 0.83 ms | 0.07 s | 418 MiB |
| 256,000 | 0.09 ms | 1.80 ms | 1.99 ms | 0.13 s | 560 MiB |
| 512,000 | 0.14 ms | 4.37 ms | 4.59 ms | 0.25 s | 958 MiB |
| 1,000,000 | 0.21 ms | 7.85 ms | 8.16 ms | 0.50 s | 1,719 MiB |
| 2,000,000 | 0.33 ms | 12.27 ms | 12.51 ms | 0.99 s | 3,193 MiB |
| 4,000,000 | 0.63 ms | 14.61 ms | 16.53 ms | 2.00 s | 6,222 MiB |
| 5,000,000 | 0.94 ms | 15.61 ms | 17.37 ms | 2.52 s | 7,737 MiB |
| 6,000,000 | 1.03 ms | 16.36 ms | 65.84 ms | 3.07 s | 9,251 MiB |
| 7,000,000 | 1.23 ms | 16.94 ms | 17.30 ms | 12.90 s | 10,767 MiB |
| 8,000,000 | 1.42 ms | 180.22 ms | 181.16 ms | 4.59 s | 16,675 MiB |

Five million points had a 50.56 ms worst frame. Six million had a 67.36 ms worst frame; a repeat produced 122.55 ms p95. Seven million had a 167.82 ms worst frame despite its relatively low p95. Eight million repeated at 180.32 ms median, confirming the severe slowdown (about 5.5 completed frames/second). No GL errors or crashes occurred at the tested sizes. Testing stopped at eight million because performance was already unusable for smooth tube orbiting.

Total VRAM climbed from about 1.4 GiB at 128,000 points to 5.7 GiB at seven million, then reported about 4.1 GiB at eight million despite much higher RSS and frame time. This suggests a driver buffer-residency/allocation cliff; residency was not independently measured, so the exact cause is unconfirmed. Raw results are in [3d-pressure-results.txt](3d-pressure-results.txt).

## Practical limits

One million points is a conservative interactive target on this PC. Two million remained below the 16.7 ms frame budget for 60 Hz in this test, but introduces about a second of synchronous tube construction. Four million approaches that budget; five to seven million show stalls; eight million is clearly too slow. More complicated designs, larger windows, other GPU workloads and different drivers can change these thresholds. The current 128,000-point cap has substantial rendering headroom.

Line rendering remained fast even at eight million points. These are rendering results; exporting multi-million-point STL files was not stress-tested. STL uses 24 triangles per point at twelve tube sides, approximately 1,200 bytes per point in the output file: one million points would produce about 1.2 GB. Render headroom alone is therefore insufficient reason to expose the same maximum to every workflow.

## Reproduce

With a working hardware OpenGL desktop session and the normal build dependencies:

```sh
./tests/pressure3d.sh 128000
./tests/pressure3d.sh 1000000
./tests/pressure3d.sh 2000000
```

The script compiles a disposable copy with a larger limit and runs one size with a 60-second timeout. Accepted counts are 1,000 through 8,000,000. Each run opens and closes its own window. Larger runs can allocate many gigabytes and stall while building the mesh. The test requires hardware OpenGL, reports generation/upload/draw times and process RSS, and queries NVIDIA memory use when `nvidia-smi` is available. Production sources and limits stay unchanged.
