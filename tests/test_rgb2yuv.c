/*
 * test_rgb2yuv.c - Self-contained unit tests for rgb2yuv library.
 * No external test framework. Exit 0 = all pass, 1 = failure.
 *
 * Test categories:
 *   1. Coefficient constraints (Y sum, CbCr sum = 0)
 *   2. Known-value point tests (3 standards x 2 ranges x 10 colors)
 *   3. Achromatic invariant (all 256 gray levels → Cb=Cr=128)
 *   4. Clamping (pure blue Cb=255, pure red Cr=255, not 256)
 *   5. I420/NV12 chroma averaging
 *   6. Odd dimensions (1x1, 1x2, 2x1, 3x3, 5x7, 7x5)
 *   7. Stride padding
 *   8. Y-plane consistency (I420 Y == YUV444 Y)
 *   9. Error handling (NULL, zero dims, bad strides)
 */

#include "rgb2yuv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------------ */
/*  Test infrastructure                                                     */
/* ------------------------------------------------------------------------ */

static int g_tests_run    = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST_ASSERT(cond, fmt, ...) do { \
    g_tests_run++; \
    if (!(cond)) { \
        g_tests_failed++; \
        fprintf(stderr, "  FAIL [%s:%d] " fmt "\n", __func__, __LINE__, ##__VA_ARGS__); \
        return; \
    } else { \
        g_tests_passed++; \
    } \
} while (0)

#define RUN_TEST(fn) do { \
    int before = g_tests_failed; \
    printf("  %-50s ", #fn); \
    fn(); \
    printf("%s\n", (g_tests_failed == before) ? "OK" : "FAIL"); \
} while (0)

/* Floating-point reference implementation for comparison */
static void ref_rgb2yuv_float(double kr, double kb,
                              int r, int g, int b,
                              int full_range,
                              int *out_y, int *out_cb, int *out_cr)
{
    double kg_val = 1.0 - kr - kb;
    double y_scale  = full_range ? 1.0 : 219.0 / 255.0;
    double uv_scale = full_range ? 1.0 : 224.0 / 255.0;
    int y_bias = full_range ? 0 : 16;

    double yf  = y_scale * (kr * r + kg_val * g + kb * b);
    double cbf = uv_scale * (-0.5 * kr / (1.0 - kb) * r
                             -0.5 * kg_val / (1.0 - kb) * g
                             + 0.5 * b);
    double crf = uv_scale * (0.5 * r
                             -0.5 * kg_val / (1.0 - kr) * g
                             -0.5 * kb / (1.0 - kr) * b);

    int yi  = (int)round(yf) + y_bias;
    int cbi = (int)round(cbf) + 128;
    int cri = (int)round(crf) + 128;

    /* Clamp */
    if (yi  < 0)   yi  = 0;
    if (yi  > 255) yi  = 255;
    if (cbi < 0)   cbi = 0;
    if (cbi > 255) cbi = 255;
    if (cri < 0)   cri = 0;
    if (cri > 255) cri = 255;

    *out_y = yi; *out_cb = cbi; *out_cr = cri;
}

static const double test_kr[] = { 0.299,  0.2126, 0.2627 };
static const double test_kb[] = { 0.114,  0.0722, 0.0593 };
static const char  *test_std_names[] = { "BT.601", "BT.709", "BT.2020" };

/* ------------------------------------------------------------------------ */
/*  Test 1: Coefficient constraints                                         */
/* ------------------------------------------------------------------------ */

static void test_coeff_constraints(void)
{
    rgb2yuv_standard stds[] = { RGB2YUV_BT601, RGB2YUV_BT709, RGB2YUV_BT2020 };
    rgb2yuv_range ranges[] = { RGB2YUV_RANGE_FULL, RGB2YUV_RANGE_STUDIO };

    for (int s = 0; s < 3; s++) {
        for (int r = 0; r < 2; r++) {
            rgb2yuv_coeffs c;
            rgb2yuv_init(&c, stds[s], ranges[r]);

            /* CbCr must sum to exactly 0 */
            int32_t cb_sum = c.cbr + c.cbg + c.cbb;
            int32_t cr_sum = c.crr + c.crg + c.crb;
            TEST_ASSERT(cb_sum == 0,
                "%s range=%d: Cb sum = %d (expected 0)", test_std_names[s], r, cb_sum);
            TEST_ASSERT(cr_sum == 0,
                "%s range=%d: Cr sum = %d (expected 0)", test_std_names[s], r, cr_sum);

            /* Y must sum to target */
            double y_scale = (r == 1) ? 219.0 / 255.0 : 1.0;
            int32_t y_target = (int32_t)round(y_scale * 65536.0);
            int32_t y_sum = c.yr + c.yg + c.yb;
            TEST_ASSERT(y_sum == y_target,
                "%s range=%d: Y sum = %d (expected %d)", test_std_names[s], r, y_sum, y_target);

            /* All Y coefficients must be positive */
            TEST_ASSERT(c.yr > 0 && c.yg > 0 && c.yb > 0,
                "%s range=%d: Y coefficients must be positive", test_std_names[s], r);
        }
    }
}

/* ------------------------------------------------------------------------ */
/*  Test 2: Known-value point tests with float reference                    */
/* ------------------------------------------------------------------------ */

typedef struct { int r, g, b; const char *name; } color_test;

static const color_test test_colors[] = {
    {   0,   0,   0, "black"     },
    { 255, 255, 255, "white"     },
    { 255,   0,   0, "red"       },
    {   0, 255,   0, "green"     },
    {   0,   0, 255, "blue"      },
    { 128, 128, 128, "mid-gray"  },
    {   1,   1,   1, "near-black"},
    { 254, 254, 254, "near-white"},
    { 200, 150, 120, "skin-tone" },
    { 255, 255,   0, "yellow"    },
};
#define N_COLORS (sizeof(test_colors) / sizeof(test_colors[0]))

static void test_known_values(void)
{
    rgb2yuv_standard stds[] = { RGB2YUV_BT601, RGB2YUV_BT709, RGB2YUV_BT2020 };
    rgb2yuv_range ranges[] = { RGB2YUV_RANGE_FULL, RGB2YUV_RANGE_STUDIO };

    for (int s = 0; s < 3; s++) {
        for (int rng = 0; rng < 2; rng++) {
            rgb2yuv_coeffs c;
            rgb2yuv_init(&c, stds[s], ranges[rng]);

            for (int i = 0; i < (int)N_COLORS; i++) {
                int tr = test_colors[i].r;
                int tg = test_colors[i].g;
                int tb = test_colors[i].b;

                /* Compute via library (YUV444 single pixel) */
                uint8_t rgb_buf[3] = { (uint8_t)tr, (uint8_t)tg, (uint8_t)tb };
                uint8_t yv, uv, vv;
                rgb2yuv_444(&c, rgb_buf, 3, &yv, 1, &uv, 1, &vv, 1, 1, 1);

                /* Compute via float reference */
                int ref_y, ref_cb, ref_cr;
                ref_rgb2yuv_float(test_kr[s], test_kb[s], tr, tg, tb,
                                  rng == 0, &ref_y, &ref_cb, &ref_cr);

                /* Allow ±1 difference (fixed-point vs float rounding) */
                TEST_ASSERT(abs(yv - ref_y) <= 1,
                    "%s rng=%d %s: Y=%d ref=%d",
                    test_std_names[s], rng, test_colors[i].name, yv, ref_y);
                TEST_ASSERT(abs(uv - ref_cb) <= 1,
                    "%s rng=%d %s: Cb=%d ref=%d",
                    test_std_names[s], rng, test_colors[i].name, uv, ref_cb);
                TEST_ASSERT(abs(vv - ref_cr) <= 1,
                    "%s rng=%d %s: Cr=%d ref=%d",
                    test_std_names[s], rng, test_colors[i].name, vv, ref_cr);
            }
        }
    }
}

/* ------------------------------------------------------------------------ */
/*  Test 3: Achromatic invariant                                            */
/* ------------------------------------------------------------------------ */

static void test_achromatic_invariant(void)
{
    rgb2yuv_standard stds[] = { RGB2YUV_BT601, RGB2YUV_BT709, RGB2YUV_BT2020 };

    for (int s = 0; s < 3; s++) {
        for (int rng = 0; rng < 2; rng++) {
            rgb2yuv_coeffs c;
            rgb2yuv_init(&c, stds[s], (rgb2yuv_range)rng);

            for (int v = 0; v <= 255; v++) {
                uint8_t rgb_buf[3] = { (uint8_t)v, (uint8_t)v, (uint8_t)v };
                uint8_t yv, uv, vv;
                rgb2yuv_444(&c, rgb_buf, 3, &yv, 1, &uv, 1, &vv, 1, 1, 1);

                TEST_ASSERT(uv == 128,
                    "%s rng=%d v=%d: Cb=%d (expected 128)",
                    test_std_names[s], rng, v, uv);
                TEST_ASSERT(vv == 128,
                    "%s rng=%d v=%d: Cr=%d (expected 128)",
                    test_std_names[s], rng, v, vv);
            }
        }
    }
}

/* ------------------------------------------------------------------------ */
/*  Test 4: Clamping — pure colors must not overflow uint8                  */
/* ------------------------------------------------------------------------ */

static void test_clamping(void)
{
    rgb2yuv_coeffs c;
    rgb2yuv_init(&c, RGB2YUV_BT601, RGB2YUV_RANGE_FULL);

    /* Pure blue → Cb should be 255 (not 256 overflow) */
    uint8_t blue[3] = { 0, 0, 255 };
    uint8_t y1, u1, v1;
    rgb2yuv_444(&c, blue, 3, &y1, 1, &u1, 1, &v1, 1, 1, 1);
    TEST_ASSERT(u1 == 255, "Pure blue Cb=%d (expected 255)", u1);

    /* Pure red → Cr should be 255 (not 256 overflow) */
    uint8_t red[3] = { 255, 0, 0 };
    uint8_t y2, u2, v2;
    rgb2yuv_444(&c, red, 3, &y2, 1, &u2, 1, &v2, 1, 1, 1);
    TEST_ASSERT(v2 == 255, "Pure red Cr=%d (expected 255)", v2);

    /* uint8_t is always [0,255] by type — no need to range-check */
}

/* ------------------------------------------------------------------------ */
/*  Test 5: I420 chroma averaging                                           */
/* ------------------------------------------------------------------------ */

static void test_i420_chroma_averaging(void)
{
    rgb2yuv_coeffs c;
    rgb2yuv_init(&c, RGB2YUV_BT601, RGB2YUV_RANGE_FULL);

    /* 2x2 image: top-left red, rest black */
    uint8_t rgb[12] = {
        255, 0, 0,    0, 0, 0,   /* row 0 */
          0, 0, 0,    0, 0, 0,   /* row 1 */
    };
    uint8_t y[4], u[1], v_buf[1];
    int rc = rgb2yuv_i420(&c, rgb, 6, y, 2, u, 1, v_buf, 1, 2, 2);
    TEST_ASSERT(rc == RGB2YUV_OK, "I420 2x2 conversion returned %d", rc);

    /* Chroma should be average of 4 pixels' CbCr */
    /* Verify Y values individually */
    uint8_t y_tl, cb_tl, cr_tl;
    rgb2yuv_444(&c, (uint8_t[]){255,0,0}, 3, &y_tl, 1, &cb_tl, 1, &cr_tl, 1, 1, 1);
    TEST_ASSERT(y[0] == y_tl, "I420 Y[0,0]=%d (expected %d)", y[0], y_tl);

    /* Black pixel Y */
    uint8_t y_bk, cb_bk, cr_bk;
    rgb2yuv_444(&c, (uint8_t[]){0,0,0}, 3, &y_bk, 1, &cb_bk, 1, &cr_bk, 1, 1, 1);
    TEST_ASSERT(y[1] == y_bk, "I420 Y[0,1]=%d (expected %d)", y[1], y_bk);

    /* Chroma: average of (red, black, black, black) */
    /* Should be close to (cb_red + 3*128) / 4 for Cb */
    /* Allow ±1 for rounding */
    int expected_cb = (int)round(((double)cb_tl + 3.0 * 128.0) / 4.0);
    int expected_cr = (int)round(((double)cr_tl + 3.0 * 128.0) / 4.0);
    TEST_ASSERT(abs((int)u[0] - expected_cb) <= 1,
        "I420 Cb=%d (expected ~%d)", u[0], expected_cb);
    TEST_ASSERT(abs((int)v_buf[0] - expected_cr) <= 1,
        "I420 Cr=%d (expected ~%d)", v_buf[0], expected_cr);
}

/* ------------------------------------------------------------------------ */
/*  Test 6: NV12 format and interleaving                                    */
/* ------------------------------------------------------------------------ */

static void test_nv12_interleaving(void)
{
    rgb2yuv_coeffs c;
    rgb2yuv_init(&c, RGB2YUV_BT601, RGB2YUV_RANGE_FULL);

    /* 2x2 uniform color → same chroma as 444 single pixel */
    uint8_t rgb[12] = {
        100, 150, 200,  100, 150, 200,
        100, 150, 200,  100, 150, 200,
    };
    uint8_t y444, u444, v444;
    rgb2yuv_444(&c, rgb, 3, &y444, 1, &u444, 1, &v444, 1, 1, 1);

    uint8_t y[4], uv[2];
    int rc = rgb2yuv_nv12(&c, rgb, 6, y, 2, uv, 2, 2, 2);
    TEST_ASSERT(rc == RGB2YUV_OK, "NV12 conversion returned %d", rc);

    /* All Y values should match 444 */
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT(y[i] == y444, "NV12 Y[%d]=%d (expected %d)", i, y[i], y444);
    }

    /* UV interleaved: [U, V] */
    TEST_ASSERT(abs((int)uv[0] - (int)u444) <= 1,
        "NV12 U=%d (expected %d)", uv[0], u444);
    TEST_ASSERT(abs((int)uv[1] - (int)v444) <= 1,
        "NV12 V=%d (expected %d)", uv[1], v444);
}

/* ------------------------------------------------------------------------ */
/*  Test 7: Odd dimensions                                                  */
/* ------------------------------------------------------------------------ */

static void test_odd_dimensions(void)
{
    rgb2yuv_coeffs c;
    rgb2yuv_init(&c, RGB2YUV_BT601, RGB2YUV_RANGE_FULL);

    /* Test various odd sizes */
    int sizes[][2] = { {1,1}, {1,2}, {2,1}, {3,3}, {5,7}, {7,5} };
    int n_sizes = sizeof(sizes) / sizeof(sizes[0]);

    for (int si = 0; si < n_sizes; si++) {
        int w = sizes[si][0], h = sizes[si][1];
        int cw_exp = (w + 1) / 2, ch_exp = (h + 1) / 2;

        /* Verify chroma_size */
        int cw, ch;
        rgb2yuv_chroma_size(w, h, &cw, &ch);
        TEST_ASSERT(cw == cw_exp && ch == ch_exp,
            "%dx%d: chroma_size=%dx%d (expected %dx%d)", w, h, cw, ch, cw_exp, ch_exp);

        /* Allocate buffers */
        uint8_t *rgb_buf = calloc((size_t)w * (size_t)h * 3, 1);
        uint8_t *y_buf = calloc((size_t)w * (size_t)h, 1);
        uint8_t *u_buf = calloc((size_t)cw * (size_t)ch, 1);
        uint8_t *v_buf = calloc((size_t)cw * (size_t)ch, 1);
        uint8_t *uv_buf = calloc((size_t)cw * 2 * (size_t)ch, 1);

        /* Fill with a known pattern */
        for (int i = 0; i < w * h * 3; i++)
            rgb_buf[i] = (uint8_t)((i * 37 + 13) & 0xFF);

        /* I420 should not crash */
        int rc = rgb2yuv_i420(&c, rgb_buf, w * 3, y_buf, w,
                              u_buf, cw, v_buf, cw, w, h);
        TEST_ASSERT(rc == RGB2YUV_OK, "%dx%d I420 returned %d", w, h, rc);

        /* NV12 should not crash */
        rc = rgb2yuv_nv12(&c, rgb_buf, w * 3, y_buf, w,
                          uv_buf, cw * 2, w, h);
        TEST_ASSERT(rc == RGB2YUV_OK, "%dx%d NV12 returned %d", w, h, rc);

        free(rgb_buf); free(y_buf); free(u_buf); free(v_buf); free(uv_buf);
    }
}

/* ------------------------------------------------------------------------ */
/*  Test 8: Stride padding                                                  */
/* ------------------------------------------------------------------------ */

static void test_stride_padding(void)
{
    rgb2yuv_coeffs c;
    rgb2yuv_init(&c, RGB2YUV_BT601, RGB2YUV_RANGE_FULL);

    int w = 4, h = 4;
    int pad = 16; /* extra padding per row */
    int rgb_stride = w * 3 + pad;
    int y_stride = w + pad;
    int uv_stride = (w / 2) + pad;

    uint8_t *rgb_buf = calloc((size_t)h * (size_t)rgb_stride, 1);
    uint8_t *y_pad = calloc((size_t)h * (size_t)y_stride, 1);
    uint8_t *u_pad = calloc((size_t)(h / 2) * (size_t)uv_stride, 1);
    uint8_t *v_pad = calloc((size_t)(h / 2) * (size_t)uv_stride, 1);
    uint8_t *y_tight = calloc((size_t)w * (size_t)h, 1);
    uint8_t *u_tight = calloc((size_t)(w / 2) * (size_t)(h / 2), 1);
    uint8_t *v_tight = calloc((size_t)(w / 2) * (size_t)(h / 2), 1);

    /* Fill RGB data (same content regardless of stride) */
    for (int row = 0; row < h; row++)
        for (int col = 0; col < w * 3; col++)
            rgb_buf[row * rgb_stride + col] = (uint8_t)((row * w * 3 + col) & 0xFF);

    /* Convert with padding */
    rgb2yuv_i420(&c, rgb_buf, rgb_stride, y_pad, y_stride,
                 u_pad, uv_stride, v_pad, uv_stride, w, h);

    /* Convert tight (create tight RGB copy first) */
    uint8_t *rgb_tight = calloc((size_t)w * (size_t)h * 3, 1);
    for (int row = 0; row < h; row++)
        memcpy(rgb_tight + row * w * 3, rgb_buf + row * rgb_stride, (size_t)w * 3);

    rgb2yuv_i420(&c, rgb_tight, w * 3, y_tight, w,
                 u_tight, w / 2, v_tight, w / 2, w, h);

    /* Compare Y planes */
    int y_match = 1;
    for (int row = 0; row < h && y_match; row++)
        for (int col = 0; col < w && y_match; col++)
            if (y_pad[row * y_stride + col] != y_tight[row * w + col])
                y_match = 0;
    TEST_ASSERT(y_match, "Padded Y plane matches tight Y plane");

    /* Compare U planes */
    int u_match = 1;
    for (int row = 0; row < h / 2 && u_match; row++)
        for (int col = 0; col < w / 2 && u_match; col++)
            if (u_pad[row * uv_stride + col] != u_tight[row * (w / 2) + col])
                u_match = 0;
    TEST_ASSERT(u_match, "Padded U plane matches tight U plane");

    free(rgb_buf); free(y_pad); free(u_pad); free(v_pad);
    free(y_tight); free(u_tight); free(v_tight); free(rgb_tight);
}

/* ------------------------------------------------------------------------ */
/*  Test 9: I420 Y-plane matches YUV444 Y-plane                            */
/* ------------------------------------------------------------------------ */

static void test_y_plane_consistency(void)
{
    rgb2yuv_coeffs c;
    rgb2yuv_init(&c, RGB2YUV_BT709, RGB2YUV_RANGE_FULL);

    int w = 8, h = 6;
    uint8_t *rgb_buf = malloc((size_t)w * (size_t)h * 3);
    uint8_t *y_444 = malloc((size_t)w * (size_t)h);
    uint8_t *u_444 = malloc((size_t)w * (size_t)h);
    uint8_t *v_444 = malloc((size_t)w * (size_t)h);
    uint8_t *y_420 = malloc((size_t)w * (size_t)h);
    uint8_t *u_420 = malloc((size_t)((w + 1) / 2) * (size_t)((h + 1) / 2));
    uint8_t *v_420 = malloc((size_t)((w + 1) / 2) * (size_t)((h + 1) / 2));

    /* Fill with pseudo-random data */
    for (int i = 0; i < w * h * 3; i++)
        rgb_buf[i] = (uint8_t)((i * 73 + 17) % 256);

    rgb2yuv_444(&c, rgb_buf, w * 3, y_444, w, u_444, w, v_444, w, w, h);
    rgb2yuv_i420(&c, rgb_buf, w * 3, y_420, w, u_420, (w + 1) / 2,
                 v_420, (w + 1) / 2, w, h);

    /* Y planes must be identical */
    int match = (memcmp(y_444, y_420, (size_t)w * (size_t)h) == 0);
    TEST_ASSERT(match, "I420 Y-plane matches YUV444 Y-plane");

    free(rgb_buf); free(y_444); free(u_444); free(v_444);
    free(y_420); free(u_420); free(v_420);
}

/* ------------------------------------------------------------------------ */
/*  Test 10: Error handling                                                 */
/* ------------------------------------------------------------------------ */

static void test_error_handling(void)
{
    rgb2yuv_coeffs c;
    rgb2yuv_init(&c, RGB2YUV_BT601, RGB2YUV_RANGE_FULL);

    uint8_t buf[12] = {0};
    uint8_t y[4], u[1], v[1];

    /* NULL coeffs */
    TEST_ASSERT(rgb2yuv_444(NULL, buf, 6, y, 2, u, 1, v, 1, 2, 2) == RGB2YUV_ERR_NULL,
        "NULL coeffs → ERR_NULL");

    /* NULL rgb */
    TEST_ASSERT(rgb2yuv_444(&c, NULL, 6, y, 2, u, 1, v, 1, 2, 2) == RGB2YUV_ERR_NULL,
        "NULL rgb → ERR_NULL");

    /* NULL y */
    TEST_ASSERT(rgb2yuv_444(&c, buf, 6, NULL, 2, u, 1, v, 1, 2, 2) == RGB2YUV_ERR_NULL,
        "NULL y → ERR_NULL");

    /* NULL u */
    TEST_ASSERT(rgb2yuv_444(&c, buf, 6, y, 2, NULL, 1, v, 1, 2, 2) == RGB2YUV_ERR_NULL,
        "NULL u → ERR_NULL");

    /* Zero dimensions */
    TEST_ASSERT(rgb2yuv_444(&c, buf, 6, y, 2, u, 1, v, 1, 0, 2) == RGB2YUV_ERR_DIMS,
        "zero width → ERR_DIMS");
    TEST_ASSERT(rgb2yuv_444(&c, buf, 6, y, 2, u, 1, v, 1, 2, 0) == RGB2YUV_ERR_DIMS,
        "zero height → ERR_DIMS");

    /* Negative dimensions */
    TEST_ASSERT(rgb2yuv_444(&c, buf, 6, y, 2, u, 1, v, 1, -1, 2) == RGB2YUV_ERR_DIMS,
        "negative width → ERR_DIMS");

    /* Stride too small */
    TEST_ASSERT(rgb2yuv_444(&c, buf, 5, y, 2, u, 1, v, 1, 2, 2) == RGB2YUV_ERR_STRIDE,
        "rgb_stride too small → ERR_STRIDE");
    TEST_ASSERT(rgb2yuv_444(&c, buf, 6, y, 1, u, 1, v, 1, 2, 2) == RGB2YUV_ERR_STRIDE,
        "y_stride too small → ERR_STRIDE");

    /* I420 chroma stride too small */
    TEST_ASSERT(rgb2yuv_i420(&c, buf, 6, y, 2, u, 0, v, 1, 2, 2) == RGB2YUV_ERR_STRIDE,
        "I420 u_stride too small → ERR_STRIDE");

    /* NV12 uv stride too small */
    uint8_t uv[2];
    TEST_ASSERT(rgb2yuv_nv12(&c, buf, 6, y, 2, uv, 1, 2, 2) == RGB2YUV_ERR_STRIDE,
        "NV12 uv_stride too small → ERR_STRIDE");
}

/* ------------------------------------------------------------------------ */
/*  Test 11: Full-range exhaustive max-error measurement                    */
/* ------------------------------------------------------------------------ */

static void test_max_error_vs_float(void)
{
    rgb2yuv_coeffs c;
    rgb2yuv_init(&c, RGB2YUV_BT601, RGB2YUV_RANGE_FULL);

    int max_y_err = 0, max_cb_err = 0, max_cr_err = 0;

    /* Test a representative subset (full 16M would take too long) */
    for (int r = 0; r <= 255; r += 5) {
        for (int g = 0; g <= 255; g += 5) {
            for (int b = 0; b <= 255; b += 5) {
                uint8_t rgb_buf[3] = { (uint8_t)r, (uint8_t)g, (uint8_t)b };
                uint8_t yv, uv, vv;
                rgb2yuv_444(&c, rgb_buf, 3, &yv, 1, &uv, 1, &vv, 1, 1, 1);

                int ref_y, ref_cb, ref_cr;
                ref_rgb2yuv_float(0.299, 0.114, r, g, b, 1,
                                  &ref_y, &ref_cb, &ref_cr);

                int ey = abs((int)yv - ref_y);
                int ecb = abs((int)uv - ref_cb);
                int ecr = abs((int)vv - ref_cr);
                if (ey > max_y_err) max_y_err = ey;
                if (ecb > max_cb_err) max_cb_err = ecb;
                if (ecr > max_cr_err) max_cr_err = ecr;
            }
        }
    }

    printf("(max err Y=%d Cb=%d Cr=%d) ", max_y_err, max_cb_err, max_cr_err);
    TEST_ASSERT(max_y_err <= 1, "Max Y error = %d (must be ≤ 1)", max_y_err);
    TEST_ASSERT(max_cb_err <= 1, "Max Cb error = %d (must be ≤ 1)", max_cb_err);
    TEST_ASSERT(max_cr_err <= 1, "Max Cr error = %d (must be ≤ 1)", max_cr_err);
}

/* ------------------------------------------------------------------------ */
/*  Test 12: Studio range boundaries                                        */
/* ------------------------------------------------------------------------ */

static void test_studio_range_boundaries(void)
{
    rgb2yuv_coeffs c;
    rgb2yuv_init(&c, RGB2YUV_BT601, RGB2YUV_RANGE_STUDIO);

    /* Black → Y = 16 */
    uint8_t black[3] = { 0, 0, 0 };
    uint8_t y1, u1, v1;
    rgb2yuv_444(&c, black, 3, &y1, 1, &u1, 1, &v1, 1, 1, 1);
    TEST_ASSERT(y1 == 16, "Studio black Y=%d (expected 16)", y1);
    TEST_ASSERT(u1 == 128, "Studio black Cb=%d (expected 128)", u1);
    TEST_ASSERT(v1 == 128, "Studio black Cr=%d (expected 128)", v1);

    /* White → Y = 235 */
    uint8_t white[3] = { 255, 255, 255 };
    uint8_t y2, u2, v2;
    rgb2yuv_444(&c, white, 3, &y2, 1, &u2, 1, &v2, 1, 1, 1);
    TEST_ASSERT(y2 == 235, "Studio white Y=%d (expected 235)", y2);
    TEST_ASSERT(u2 == 128, "Studio white Cb=%d (expected 128)", u2);
    TEST_ASSERT(v2 == 128, "Studio white Cr=%d (expected 128)", v2);
}

/* ------------------------------------------------------------------------ */
/*  Main                                                                    */
/* ------------------------------------------------------------------------ */

int main(void)
{
    printf("rgb2yuv unit tests\n");
    printf("==================\n\n");

    printf("[Coefficient tests]\n");
    RUN_TEST(test_coeff_constraints);

    printf("\n[Point tests]\n");
    RUN_TEST(test_known_values);
    RUN_TEST(test_studio_range_boundaries);

    printf("\n[Invariant tests]\n");
    RUN_TEST(test_achromatic_invariant);
    RUN_TEST(test_clamping);

    printf("\n[Format tests]\n");
    RUN_TEST(test_i420_chroma_averaging);
    RUN_TEST(test_nv12_interleaving);
    RUN_TEST(test_y_plane_consistency);

    printf("\n[Edge case tests]\n");
    RUN_TEST(test_odd_dimensions);
    RUN_TEST(test_stride_padding);

    printf("\n[Error handling tests]\n");
    RUN_TEST(test_error_handling);

    printf("\n[Precision tests]\n");
    RUN_TEST(test_max_error_vs_float);

    printf("\n==================\n");
    printf("Results: %d/%d passed, %d failed\n",
           g_tests_passed, g_tests_run, g_tests_failed);

    return g_tests_failed > 0 ? 1 : 0;
}
