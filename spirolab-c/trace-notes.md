# Trace Implementation Notes (`trace.c`)

### 1. Mask Tracing (`sp_trace_mask`)
- **Thinning**: Zhang-Suen algorithm operated on a 1-pixel zero-padded buffer to eliminate edge-boundary checks and prevent boundary artifacts.
- **Blob Preservation**: Preserves small isolated specks/squares (e.g., 2x2 blocks) that could thin away by detecting unrepresented connected components and restoring their centroid pixel.
- **Component Walking**: Computes 8-neighbor degrees. Traverses along strokes by preferring minimum-degree unvisited neighbors. Connects disconnected components deliberately to the nearest unvisited endpoint (or nearest point if closed loops).
- **Duplicate Removal**: Strips consecutive duplicate coordinates.
- **Normalization**: Preserves aspect ratio by scaling maximum dimension span to $[-0.85, 0.85]$ centered at the bounding box centroid. Degenerate single-pixel masks map cleanly to $(0.0, 0.0)$. Empty masks return 0 and reset output.

### 2. Fourier Analysis (`sp_fourier`)
- **Resampling**: Treats polyline as closed by including the segment between the final and initial point. Parameterizes path by cumulative length and resamples uniformly into power-of-two sample counts (16..16384).
- **FFT**: In-place Cooley-Tukey radix-2 FFT (sign = -1). Normalizes coefficients by sample count $N$.
- **Frequencies & Sorting**: Assigns standard signed frequencies ($0, 1 \dots N/2 - 1, -N/2, -N/2 + 1 \dots -1$). Sorts terms in descending order of complex modulus (amplitude). Rejects non-finite coordinates and zero-length degenerate inputs.

### 3. Synthesis & Lifecycle (`sp_synthesize`, `sp_fourier_free`)
- **Synthesis**: Sums harmonic terms across continuous normalized time $t = s / (\text{samples} - 1)$ for $s \in [0, \text{samples} - 1]$, ensuring exact mathematical closure at the final sample. Clamps requested terms and enforces sample bounds in $[2, 250000]$.
- **Deallocation**: `sp_fourier_free` cleanly deallocates term arrays and zeroes counts. `sp_path_free` is omitted as instructed to prevent duplicate linkage.
