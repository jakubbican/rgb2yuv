# rgb2yuv — RGB to YUV Color Space Converter

Fast, correct, portable RGB to YUV conversion library and CLI tool.

Supports BT.601, BT.709, BT.2020 with full and studio range output.
Three output formats: YUV444 (planar), I420 (planar 4:2:0), NV12 (semi-planar 4:2:0).

## Build

```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

Requires: C99 compiler, CMake 3.10+. No external dependencies (stb_image bundled).

## CLI Usage

```bash
# Convert PNG to I420 (default)
./rgb2yuv input.png output.yuv

# BT.709 studio range NV12
./rgb2yuv -s bt709 -r studio -f nv12 input.png output.yuv

# Show coefficients
./rgb2yuv --info -s bt2020 -r full
```

Options:
- `-s, --standard <bt601|bt709|bt2020>` — Color standard (default: bt601)
- `-r, --range <full|studio>` — Value range (default: full)
- `-f, --format <yuv444|i420|nv12>` — Output format (default: i420)
- `--info` — Print conversion parameters and exit

Input: PNG, JPEG, BMP, TGA. Output: raw YUV (Y plane followed by U/V or UV).

## Library API

```c
#include "rgb2yuv.h"

rgb2yuv_coeffs coeffs;
rgb2yuv_init(&coeffs, RGB2YUV_BT709, RGB2YUV_RANGE_FULL);

int rc = rgb2yuv_i420(&coeffs, rgb, width * 3,
                      y, width, u, cw, v, cw,
                      width, height);
```

Thread-safe: `rgb2yuv_coeffs` is read-only after init. Conversion functions are stateless.

## Algorithm

- **16-bit fixed-point** arithmetic (shift = 16, scale = 65536)
- Coefficients derived from ITU-R standards with **algebraic constraint enforcement**:
  - Y coefficients sum to exact scale factor (achromatic: white → Y=255/235 exactly)
  - CbCr coefficients sum to exactly 0 (achromatic: gray → Cb=Cr=128 exactly)
- **Single-pass 2-row stripe** processing for 4:2:0 (each pixel read once)
- **Sum-then-convert** chroma subsampling (single rounding, mathematically equivalent to convert-then-average for linear transforms)
- Maximum error vs. float reference: ±1 LSB (measured across all 16.7M RGB combinations)
- Correct handling of **odd image dimensions** for I420/NV12

## Performance (ARM Cortex-A76 @ 2.4 GHz, scalar, -O2)

| Resolution | YUV444 | I420 | NV12 |
|------------|--------|------|------|
| 640x480    | 215 Mpix/s | 410 Mpix/s | 400 Mpix/s |
| 1920x1080  | 215 Mpix/s | 365 Mpix/s | 365 Mpix/s |
| 3840x2160  | 215 Mpix/s | 377 Mpix/s | 376 Mpix/s |

I420/NV12 is ~1.7× faster than YUV444 (fewer CbCr computations per pixel).

## Tests

```bash
./build/test_rgb2yuv    # 3331 unit tests
./build/bench_rgb2yuv   # throughput + precision benchmark
```

Test coverage: coefficient constraints, known-value point tests (3 standards × 2 ranges × 10 colors), achromatic invariant (all 256 gray levels), clamping, chroma averaging, odd dimensions, stride padding, Y-plane consistency, error handling, max-error measurement.

## Optional: FFmpeg cross-validation

```bash
# Generate reference files (requires ffmpeg)
./tests/gen_reference.sh

# Compare output
./build/rgb2yuv -s bt601 -r studio -f i420 tests/ref/smpte_640x480.png /tmp/our.i420
cmp tests/ref/smpte_640x480_bt601_studio.i420 /tmp/our.i420
```

## SIMD Extensions (Future)

Current version is **portable scalar** code optimized for auto-vectorization (`restrict`, branchless clamp, simple loop structure). The `rgb2yuv_coeffs` struct is designed for future extension with:

- **ARM NEON** — `vld3_u8` for RGB deinterleave, `vmull/vmlal` for MAC, `vqmovn` for saturating narrow
- **x86 SSE4.1 / AVX2** — `_mm_shuffle_epi8` for deinterleave, `_mm_madd_epi16` for packed MAC
- **Runtime dispatch** — function pointers in an extended coeffs struct, selected at init based on CPU features

## Design Decisions

See [PLAN.md](PLAN.md) for the full design rationale and [PROTOCOL.md](PROTOCOL.md) for the development protocol, including:

- Why gamma-domain processing (industry standard, not a bug)
- Why box filter for chroma siting (MPEG-1 center-aligned)
- Why 16-bit fixed-point (40× lower coefficient error than popular 8-bit approximation)
- Why fixed-point multiply, not LUTs
- Why single-pass, not two-pass for 4:2:0

## License

MIT
