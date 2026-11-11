# Native SpiroLab port — 2026-11-11

Source reference: the web application at commit `71acc31`, and the canonical SpiroLab/local-agent-swarm vault notes read fresh on arrstack. Source and executable live in this separate `spirolab-c/` directory.

## Implementation

C11 executable with GTK3 widgets, Cairo paths/exports, Pango text rasterization, GdkPixbuf image decoding, and the ONNX Runtime C API. Curve samples and trained-model inference are local. No browser, JavaScript interpreter or Node server is embedded.

| Module | Responsibility |
|---|---|
| `engine.c` | Eleven shape generators, centroid/arc length, rolling and fillets, closure, analytic circle gears and second wheel |
| `trace.c` | Thinning, stroke walking, Fourier analysis, direct or inverse FFT synthesis |
| `image.c` | Text rasterization, edge/ink extraction, thin-stroke preservation and pen-lift timing |
| `ai.c` | U2-Net segmentation, SCRFD detection/NMS,106 landmarks and face/subject outlines |
| `render.c` | Stable ink, pen lifts, viewport, gear/Fourier overlays, PNG/SVG/PDF |
| `document.c` | Atomic saves; bounded transactional loads including paths, inks, pen lifts, coefficients and embedded PNGs |
| `desktop.c` | Native controls, background tasks, image tools, lifecycle and headless commands |
| `spiro3d.c` | Chained 3D arm rotations, 2D Fourier lift, closed parallel-transport tube mesh and binary STL |
| `spiro3d_io.c` | Perspective line/shaded-mesh rendering, 3D design files and PNG/STL export |
| `viewer3d.c` | Native 3D window, arm table, orbit/pan/zoom and display-clock playback |
| `gpu3d.c` | Cached OpenGL buffers, perspective shaders, smooth tube lighting, depth/MSAA, profiling and readback |

The interface and file handling were completed in Codex. The original server/web deployments are separate. The canonical vault now documents this application in `ian-pc.md`, `spirolab.md`, `local-agent-swarm.md` and `recent-changes.md`; arruser ownership was restored after the updates.

## Coverage

Implemented:2D geometry, inside/outside tooth sizing, pen/rotation and second inside wheel; playback, zoom/pan/fit, presets/shuffle,32 layers; text/image tracing and Fourier circles; local face drawing, transparent cutout and subject outline; PNG/SVG/PDF; local design files. Limits and the remaining music/video/server-tool parity gaps are listed in README.md.

## Swarm provenance and quota

Four isolated Antigravity/Gemini Flash High workers received separate scratch folders, small shared C interfaces, relevant source excerpts and explicit ownership. They produced geometry, tracing, desktop/rendering and AI drafts. No recursive workers were requested. Scratch contracts, pinned checks and results are under `../contracts/spirolab-c/`.

The desktop first pass reached its14-minute timeout after writing source. Two focused Flash Medium review dispatches were attempted. Both returned `RESOURCE_EXHAUSTED`/HTTP429 without changes; the AI first pass also ended with an individual-quota error after writing code. Reported reset was roughly167h44m. Initial remaining quota and exact token totals were unavailable, so the fraction consumed by this session is unknown.

No more AGY calls followed. The user explicitly authorized Codex to override gemini-dev's requirement that Gemini write corrections. Codex fixed allocation cleanup and parameter bounds; checked ONNX statuses/types/shapes; rewrote the desktop integration and persistence; added fast synthesis, pen lifts, previews, background tasks and launcher; and ran local validation.

Time limits and short contracts were not sufficient token/turn limits: high-effort workers spent many analysis/tool turns before writing. Future runs should use smaller implementation slices/lower effort, inspect available quota when possible, and monitor intermediate progress.

## Verification

- Clean optimized C11 build with `-Wall -Wextra -Wpedantic`; own code also compiled with `-Werror`.
- All242 shape/side combinations finite and closed. Eight circle gear configurations agree with independent analytic formulas to at most4.8e-14.
- FFT reconstruction, disconnected masks,16384-term text rendering, preserved thin line art and pen-up travel.
- Stable layer colour; failed SVG/PDF writes report failure; gear overlay agrees with the computed pen.
- Design roundtrip includes separate current/control inks, source/prepared images, Fourier terms and pen lifts. Corrupt Current data preserves the previous document.
- ASan/UBSan and injected allocation-failure checks.
- Real local ONNX inference: cutout, silhouette, face and face+outline. Public upstream test image detected6 faces; missing-model errors validated.
- Actual GTK callbacks exercise gears, text, Fourier overlay, face preparation/tracing, layers and close-during-inference, with fatal GTK assertions enabled. Visual review caught and corrected thin-stroke loss and missing pen-lift handling.


## Native 3D addition — 2026-11-11

Implemented directly in Codex with no further AGY calls. Main toolbar opens **3D Spiro**; `--3d`, `--render-3d` and `--stl-3d` support direct/headless use. Includes1–16 arms, signed integer speeds, tilt/azimuth/phase, line and shaded tube views, orbit/pan/zoom, camera spin, arm overlay, playback/replay, randomization, 2D gear-pattern Fourier lift, saved3D/camera state,2048px PNG and closed binary STL in mm. Existing dependencies suffice.

Verification: compared C points to the unchanged original3d.html geometry with independent quaternion rotation; maximum discrepancy7.45e-16. Analytic one/two-arm and tilted-axis cases,16-arm periodic closure and joint/pen agreement pass. Tube topology has exactly two oppositely oriented faces at each edge; a circular centreline's signed volume agrees with its polygonal torus volume. STL byte length/count, camera-independent sizing, saved-data roundtrip/corrupt-load preservation and write failures pass. ASan/UBSan3D checks pass. GTK tests exercise arm changes/limits, invalid settings, lift, shaded view, orbit/pan/zoom, playback and cleanup, with fatal GTK assertions enabled. Full existing regression suite passes.

Music and video/turntable export are not included in this addition.


## OpenGL orbit performance — 2026-11-11

Replaced per-frame Cairo projection, triangle rebuilding and sorting in the interactive 3D viewport with GtkGLArea/OpenGL 3.3 via the existing Epoxy library. Geometry resides in retained vertex/index buffers. Camera/hue/playback updates use uniforms/draw ranges; tube buffers rebuild only for curve or radius/physical-scale changes. Hardware uses full selected meshes, depth testing, smooth per-vertex lighting and 4x MSAA. Software OpenGL uses a decimated preview index buffer and 2x MSAA. Frame-clock callbacks replace the 33 ms animation timer. Cairo remains the context-failure fallback and headless PNG path; STL math is unchanged.

The OpenGL integration test checks actual pixels against an analytic circle, repeated finished-curve orbit with invariant upload counts, hue/zoom/mouse input, playback, arm overlay, radius-triggered rebuilds,32000 samples, framebuffer resize and destruction. Explicit unavailable-GL fallback, existing GUI/regression checks and ASan/UBSan GPU tests were exercised (GPU test leak detection disabled for process-wide GTK/Mesa caches).

Hardware verification is blocked by the PC's active NVIDIA mismatch: loaded 615.78.08 module on the running 7.3.0-4 kernel, user-space nvidia-utils 620.70.06-2. The installed 7.3.3-1 kernel contains matching 620.70.06 modules. No reboot or driver package changes were performed. Default Wayland startup selected llvmpipe and correctly reported software rendering. Explicit Mesa validation measured approximately 15.1 ms per completed-tube orbit frame versus 118.5 ms for Cairo at the same 758×762 viewport, with geometry upload counts unchanged. These are software-renderer measurements, not NVIDIA GPU benchmarks. Reboot/reopen, then inspect the status or run tests/gpu.sh to verify hardware acceleration.
