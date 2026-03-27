/*
 * rgb2yuv.c - RGB to YUV color space conversion
 *
 * Fixed-point 16-bit arithmetic. Coefficients derived from ITU-R BT.601/709/2020.
 * See PLAN.md for algorithmic rationale and overflow analysis.
 */

#include "rgb2yuv.h"
#include <math.h>   /* round() - used only in rgb2yuv_init, not in hot path */
#include <stddef.h> /* NULL */

/* ------------------------------------------------------------------------ */
/*  Constants                                                               */
/* ------------------------------------------------------------------------ */

#define SHIFT    16
#define SCALE    (1 << SHIFT)          /* 65536 */
#define HALF     (1 << (SHIFT - 1))    /* 32768 */

/* ------------------------------------------------------------------------ */
/*  Helpers (static inline — no function-call overhead, auto-vec friendly)  */
/* ------------------------------------------------------------------------ */

/*
 * Clamp int32_t to [0, 255]. Written as ternary for compiler to emit
 * CMOV (x86) or CSEL (ARM) — branchless on modern CPUs.
 * Only needed for Cb/Cr; Y is mathematically proven to stay in range.
 */
static inline uint8_t clamp_u8(int32_t v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

/*
 * Single-pixel RGB → Y conversion. No clamp needed (proven in-range).
 * All pointer params in the caller use restrict, so the compiler knows
 * there's no aliasing.
 */
static inline uint8_t compute_y(const rgb2yuv_coeffs *c,
                                int32_t r, int32_t g, int32_t b)
{
    return (uint8_t)((c->yr * r + c->yg * g + c->yb * b + c->y_add) >> SHIFT);
}

/* Single-pixel RGB → Cb. Needs clamp (can reach 256 for pure blue). */
static inline uint8_t compute_cb(const rgb2yuv_coeffs *c,
                                 int32_t r, int32_t g, int32_t b)
{
    return clamp_u8((c->cbr * r + c->cbg * g + c->cbb * b + c->c_add) >> SHIFT);
}

/* Single-pixel RGB → Cr. Needs clamp (can reach 256 for pure red). */
static inline uint8_t compute_cr(const rgb2yuv_coeffs *c,
                                 int32_t r, int32_t g, int32_t b)
{
    return clamp_u8((c->crr * r + c->crg * g + c->crb * b + c->c_add) >> SHIFT);
}

/* ------------------------------------------------------------------------ */
/*  Input validation                                                        */
/* ------------------------------------------------------------------------ */

/* Validate common parameters. Returns RGB2YUV_OK or negative error. */
static int validate_common(const rgb2yuv_coeffs *coeffs,
                           const uint8_t *rgb, int rgb_stride,
                           const uint8_t *y, int y_stride,
                           int width, int height)
{
    if (!coeffs || !rgb || !y)
        return RGB2YUV_ERR_NULL;
    if (width <= 0 || height <= 0)
        return RGB2YUV_ERR_DIMS;
    /* Prevent overflow in width * 3 */
    if (width > 715827882) /* INT32_MAX / 3 */
        return RGB2YUV_ERR_DIMS;
    if (rgb_stride < width * 3)
        return RGB2YUV_ERR_STRIDE;
    if (y_stride < width)
        return RGB2YUV_ERR_STRIDE;
    return RGB2YUV_OK;
}

/* ------------------------------------------------------------------------ */
/*  rgb2yuv_init                                                            */
/* ------------------------------------------------------------------------ */

/*
 * ITU-R standard parameters: (Kr, Kb) pairs.
 * Kg is derived: Kg = 1 - Kr - Kb.
 */
static const double std_kr[] = { 0.299,  0.2126, 0.2627 };
static const double std_kb[] = { 0.114,  0.0722, 0.0593 };

void rgb2yuv_init(rgb2yuv_coeffs *coeffs,
                  rgb2yuv_standard standard,
                  rgb2yuv_range range)
{
    if (!coeffs) return;

    /* Clamp enum to valid range */
    int si = (int)standard;
    if (si < 0 || si > 2) si = 0;

    double kr = std_kr[si];
    double kb = std_kb[si];
    double kg = 1.0 - kr - kb;

    /* Scale factors: full range uses 1.0, studio scales Y by 219/255, CbCr by 224/255 */
    double y_scale  = (range == RGB2YUV_RANGE_STUDIO) ? 219.0 / 255.0 : 1.0;
    double uv_scale = (range == RGB2YUV_RANGE_STUDIO) ? 224.0 / 255.0 : 1.0;

    /*
     * Y coefficients.
     * Constraint: yr + yg + yb = y_target (exact).
     * Round two, compute third to enforce the constraint.
     * This guarantees: white (255,255,255) → exact Y=255 (full) or Y=235 (studio).
     */
    int32_t y_target = (int32_t)round(y_scale * (double)SCALE);
    coeffs->yr = (int32_t)round(kr * y_scale * (double)SCALE);
    coeffs->yg = (int32_t)round(kg * y_scale * (double)SCALE);
    coeffs->yb = y_target - coeffs->yr - coeffs->yg;  /* enforce sum */

    /*
     * Cb coefficients: Cb = [-0.5*Kr/(1-Kb), -0.5*Kg/(1-Kb), 0.5] * uv_scale
     * Constraint: cbr + cbg + cbb = 0 (exact).
     * Guarantees achromatic (R=G=B) → Cb = 128 exactly.
     */
    coeffs->cbb = (int32_t)round(0.5 * uv_scale * (double)SCALE);
    coeffs->cbr = (int32_t)round(-0.5 * kr / (1.0 - kb) * uv_scale * (double)SCALE);
    coeffs->cbg = -(coeffs->cbr + coeffs->cbb);  /* enforce sum = 0 */

    /*
     * Cr coefficients: Cr = [0.5, -0.5*Kg/(1-Kr), -0.5*Kb/(1-Kr)] * uv_scale
     * Constraint: crr + crg + crb = 0 (exact).
     */
    coeffs->crr = (int32_t)round(0.5 * uv_scale * (double)SCALE);
    coeffs->crg = (int32_t)round(-0.5 * kg / (1.0 - kr) * uv_scale * (double)SCALE);
    coeffs->crb = -(coeffs->crr + coeffs->crg);  /* enforce sum = 0 */

    /*
     * Pre-computed add constants (rounding + offset folded together).
     * This eliminates one addition per pixel in the hot loop.
     *
     * For Y:    y_add = y_bias * SCALE + HALF
     *           y_bias = 16 for studio, 0 for full
     * For CbCr: c_add = 128 * SCALE + HALF
     *           The +128 offset shifts [-128,127] → [0,255].
     *           HALF provides round-half-up for the >> SHIFT.
     */
    int32_t y_bias = (range == RGB2YUV_RANGE_STUDIO) ? 16 : 0;
    coeffs->y_add = y_bias * SCALE + HALF;
    coeffs->c_add = 128 * SCALE + HALF;
}

/* ------------------------------------------------------------------------ */
/*  rgb2yuv_chroma_size                                                     */
/* ------------------------------------------------------------------------ */

void rgb2yuv_chroma_size(int width, int height, int *cw, int *ch)
{
    if (cw) *cw = (width  + 1) / 2;
    if (ch) *ch = (height + 1) / 2;
}

/* ------------------------------------------------------------------------ */
/*  rgb2yuv_444                                                             */
/* ------------------------------------------------------------------------ */

int rgb2yuv_444(const rgb2yuv_coeffs *coeffs,
                const uint8_t * restrict rgb, int rgb_stride,
                uint8_t * restrict y, int y_stride,
                uint8_t * restrict u, int u_stride,
                uint8_t * restrict v, int v_stride,
                int width, int height)
{
    int err = validate_common(coeffs, rgb, rgb_stride, y, y_stride, width, height);
    if (err) return err;
    if (!u || !v)
        return RGB2YUV_ERR_NULL;
    if (u_stride < width || v_stride < width)
        return RGB2YUV_ERR_STRIDE;

    for (int row = 0; row < height; row++) {
        const uint8_t * restrict src = rgb + row * rgb_stride;
        uint8_t * restrict yp = y + row * y_stride;
        uint8_t * restrict up = u + row * u_stride;
        uint8_t * restrict vp = v + row * v_stride;

        for (int col = 0; col < width; col++) {
            int32_t r = src[0];
            int32_t g = src[1];
            int32_t b = src[2];
            src += 3;

            yp[col] = compute_y(coeffs, r, g, b);
            up[col] = compute_cb(coeffs, r, g, b);
            vp[col] = compute_cr(coeffs, r, g, b);
        }
    }
    return RGB2YUV_OK;
}

/* ------------------------------------------------------------------------ */
/*  rgb2yuv_i420 — single-pass 2-row stripe processing                     */
/* ------------------------------------------------------------------------ */

/*
 * Chroma from a sum of N pixels (N = 1, 2, or 4).
 * Uses combined shift: >> (SHIFT + log2(N)).
 *
 *   For N=4: (coeff * sum_of_4 + 128 * SCALE * 4 + HALF * 4) >> (SHIFT + 2)
 *   For N=2: (coeff * sum_of_2 + 128 * SCALE * 2 + HALF * 2) >> (SHIFT + 1)
 *   For N=1: standard single-pixel formula
 *
 * This avoids double rounding (averaging first, then converting).
 * Overflow: max |32768 * 1020| = 33.4M, + offset ~33.7M = ~67M. int32 OK.
 */

/* Pre-computed offsets for different block sizes */
#define C_ADD_4  (128 * SCALE * 4 + HALF * 4)  /* 33685504 for >>18 */
#define C_ADD_2  (128 * SCALE * 2 + HALF * 2)  /* 16842752 for >>17 */

int rgb2yuv_i420(const rgb2yuv_coeffs *coeffs,
                 const uint8_t * restrict rgb, int rgb_stride,
                 uint8_t * restrict y, int y_stride,
                 uint8_t * restrict u, int u_stride,
                 uint8_t * restrict v, int v_stride,
                 int width, int height)
{
    int err = validate_common(coeffs, rgb, rgb_stride, y, y_stride, width, height);
    if (err) return err;
    if (!u || !v)
        return RGB2YUV_ERR_NULL;

    int cw = (width + 1) / 2;
    if (u_stride < cw || v_stride < cw)
        return RGB2YUV_ERR_STRIDE;

    int even_w = width & ~1;
    int even_h = height & ~1;

    /* Process pairs of rows */
    for (int row = 0; row < even_h; row += 2) {
        const uint8_t * restrict src0 = rgb + row * rgb_stride;
        const uint8_t * restrict src1 = rgb + (row + 1) * rgb_stride;
        uint8_t * restrict yp0 = y + row * y_stride;
        uint8_t * restrict yp1 = y + (row + 1) * y_stride;
        uint8_t * restrict up  = u + (row / 2) * u_stride;
        uint8_t * restrict vp  = v + (row / 2) * v_stride;

        int cx = 0;

        /* Process pairs of columns (2x2 blocks) */
        for (int col = 0; col < even_w; col += 2, cx++) {
            /* Read 4 pixels */
            int32_t r00 = src0[0], g00 = src0[1], b00 = src0[2];
            int32_t r01 = src0[3], g01 = src0[4], b01 = src0[5];
            int32_t r10 = src1[0], g10 = src1[1], b10 = src1[2];
            int32_t r11 = src1[3], g11 = src1[4], b11 = src1[5];
            src0 += 6;
            src1 += 6;

            /* Y for all 4 pixels */
            yp0[col]     = compute_y(coeffs, r00, g00, b00);
            yp0[col + 1] = compute_y(coeffs, r01, g01, b01);
            yp1[col]     = compute_y(coeffs, r10, g10, b10);
            yp1[col + 1] = compute_y(coeffs, r11, g11, b11);

            /* Sum-then-convert for chroma (single rounding) */
            int32_t rs = r00 + r01 + r10 + r11;
            int32_t gs = g00 + g01 + g10 + g11;
            int32_t bs = b00 + b01 + b10 + b11;

            up[cx] = clamp_u8((coeffs->cbr * rs + coeffs->cbg * gs
                              + coeffs->cbb * bs + C_ADD_4) >> (SHIFT + 2));
            vp[cx] = clamp_u8((coeffs->crr * rs + coeffs->crg * gs
                              + coeffs->crb * bs + C_ADD_4) >> (SHIFT + 2));
        }

        /* Odd last column: 2x1 block (2 pixels vertically) */
        if (width & 1) {
            int32_t r00 = src0[0], g00 = src0[1], b00 = src0[2];
            int32_t r10 = src1[0], g10 = src1[1], b10 = src1[2];

            yp0[even_w] = compute_y(coeffs, r00, g00, b00);
            yp1[even_w] = compute_y(coeffs, r10, g10, b10);

            int32_t rs = r00 + r10;
            int32_t gs = g00 + g10;
            int32_t bs = b00 + b10;

            up[cx] = clamp_u8((coeffs->cbr * rs + coeffs->cbg * gs
                              + coeffs->cbb * bs + C_ADD_2) >> (SHIFT + 1));
            vp[cx] = clamp_u8((coeffs->crr * rs + coeffs->crg * gs
                              + coeffs->crb * bs + C_ADD_2) >> (SHIFT + 1));
        }
    }

    /* Odd last row: 1x2 blocks (and possibly 1x1 corner) */
    if (height & 1) {
        int last_row = even_h;
        const uint8_t * restrict src0 = rgb + last_row * rgb_stride;
        uint8_t * restrict yp0 = y + last_row * y_stride;
        uint8_t * restrict up  = u + (last_row / 2) * u_stride;
        uint8_t * restrict vp  = v + (last_row / 2) * v_stride;

        int cx = 0;

        for (int col = 0; col < even_w; col += 2, cx++) {
            int32_t r00 = src0[0], g00 = src0[1], b00 = src0[2];
            int32_t r01 = src0[3], g01 = src0[4], b01 = src0[5];
            src0 += 6;

            yp0[col]     = compute_y(coeffs, r00, g00, b00);
            yp0[col + 1] = compute_y(coeffs, r01, g01, b01);

            int32_t rs = r00 + r01;
            int32_t gs = g00 + g01;
            int32_t bs = b00 + b01;

            up[cx] = clamp_u8((coeffs->cbr * rs + coeffs->cbg * gs
                              + coeffs->cbb * bs + C_ADD_2) >> (SHIFT + 1));
            vp[cx] = clamp_u8((coeffs->crr * rs + coeffs->crg * gs
                              + coeffs->crb * bs + C_ADD_2) >> (SHIFT + 1));
        }

        /* 1x1 corner pixel */
        if (width & 1) {
            int32_t r = src0[0], g = src0[1], b = src0[2];
            yp0[even_w] = compute_y(coeffs, r, g, b);
            up[cx] = compute_cb(coeffs, r, g, b);
            vp[cx] = compute_cr(coeffs, r, g, b);
        }
    }

    return RGB2YUV_OK;
}

/* ------------------------------------------------------------------------ */
/*  rgb2yuv_nv12 — same as I420 but interleaved UV output                   */
/* ------------------------------------------------------------------------ */

int rgb2yuv_nv12(const rgb2yuv_coeffs *coeffs,
                 const uint8_t * restrict rgb, int rgb_stride,
                 uint8_t * restrict y, int y_stride,
                 uint8_t * restrict uv, int uv_stride,
                 int width, int height)
{
    int err = validate_common(coeffs, rgb, rgb_stride, y, y_stride, width, height);
    if (err) return err;
    if (!uv)
        return RGB2YUV_ERR_NULL;

    int cw = (width + 1) / 2;
    if (uv_stride < cw * 2)
        return RGB2YUV_ERR_STRIDE;

    int even_w = width & ~1;
    int even_h = height & ~1;

    for (int row = 0; row < even_h; row += 2) {
        const uint8_t * restrict src0 = rgb + row * rgb_stride;
        const uint8_t * restrict src1 = rgb + (row + 1) * rgb_stride;
        uint8_t * restrict yp0  = y  + row * y_stride;
        uint8_t * restrict yp1  = y  + (row + 1) * y_stride;
        uint8_t * restrict uvp  = uv + (row / 2) * uv_stride;

        int cx = 0;

        for (int col = 0; col < even_w; col += 2, cx += 2) {
            int32_t r00 = src0[0], g00 = src0[1], b00 = src0[2];
            int32_t r01 = src0[3], g01 = src0[4], b01 = src0[5];
            int32_t r10 = src1[0], g10 = src1[1], b10 = src1[2];
            int32_t r11 = src1[3], g11 = src1[4], b11 = src1[5];
            src0 += 6;
            src1 += 6;

            yp0[col]     = compute_y(coeffs, r00, g00, b00);
            yp0[col + 1] = compute_y(coeffs, r01, g01, b01);
            yp1[col]     = compute_y(coeffs, r10, g10, b10);
            yp1[col + 1] = compute_y(coeffs, r11, g11, b11);

            int32_t rs = r00 + r01 + r10 + r11;
            int32_t gs = g00 + g01 + g10 + g11;
            int32_t bs = b00 + b01 + b10 + b11;

            uvp[cx]     = clamp_u8((coeffs->cbr * rs + coeffs->cbg * gs
                                   + coeffs->cbb * bs + C_ADD_4) >> (SHIFT + 2));
            uvp[cx + 1] = clamp_u8((coeffs->crr * rs + coeffs->crg * gs
                                   + coeffs->crb * bs + C_ADD_4) >> (SHIFT + 2));
        }

        if (width & 1) {
            int32_t r00 = src0[0], g00 = src0[1], b00 = src0[2];
            int32_t r10 = src1[0], g10 = src1[1], b10 = src1[2];

            yp0[even_w] = compute_y(coeffs, r00, g00, b00);
            yp1[even_w] = compute_y(coeffs, r10, g10, b10);

            int32_t rs = r00 + r10;
            int32_t gs = g00 + g10;
            int32_t bs = b00 + b10;

            uvp[cx]     = clamp_u8((coeffs->cbr * rs + coeffs->cbg * gs
                                   + coeffs->cbb * bs + C_ADD_2) >> (SHIFT + 1));
            uvp[cx + 1] = clamp_u8((coeffs->crr * rs + coeffs->crg * gs
                                   + coeffs->crb * bs + C_ADD_2) >> (SHIFT + 1));
        }
    }

    if (height & 1) {
        int last_row = even_h;
        const uint8_t * restrict src0 = rgb + last_row * rgb_stride;
        uint8_t * restrict yp0 = y  + last_row * y_stride;
        uint8_t * restrict uvp = uv + (last_row / 2) * uv_stride;

        int cx = 0;

        for (int col = 0; col < even_w; col += 2, cx += 2) {
            int32_t r00 = src0[0], g00 = src0[1], b00 = src0[2];
            int32_t r01 = src0[3], g01 = src0[4], b01 = src0[5];
            src0 += 6;

            yp0[col]     = compute_y(coeffs, r00, g00, b00);
            yp0[col + 1] = compute_y(coeffs, r01, g01, b01);

            int32_t rs = r00 + r01;
            int32_t gs = g00 + g01;
            int32_t bs = b00 + b01;

            uvp[cx]     = clamp_u8((coeffs->cbr * rs + coeffs->cbg * gs
                                   + coeffs->cbb * bs + C_ADD_2) >> (SHIFT + 1));
            uvp[cx + 1] = clamp_u8((coeffs->crr * rs + coeffs->crg * gs
                                   + coeffs->crb * bs + C_ADD_2) >> (SHIFT + 1));
        }

        if (width & 1) {
            int32_t r = src0[0], g = src0[1], b = src0[2];
            yp0[even_w] = compute_y(coeffs, r, g, b);
            uvp[cx]     = compute_cb(coeffs, r, g, b);
            uvp[cx + 1] = compute_cr(coeffs, r, g, b);
        }
    }

    return RGB2YUV_OK;
}
