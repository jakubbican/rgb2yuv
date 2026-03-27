/*
 * rgb2yuv.h - RGB to YUV color space conversion library
 *
 * Supports BT.601, BT.709, BT.2020 standards with full/studio range.
 * Output formats: YUV444 (planar), I420 (planar 4:2:0), NV12 (semi-planar 4:2:0).
 *
 * All conversion uses 16-bit fixed-point arithmetic for accuracy and speed.
 * Coefficients are derived from ITU-R standards with algebraic constraint
 * enforcement (Y sum = exact scale, CbCr sum = 0).
 *
 * Thread safety: rgb2yuv_coeffs is read-only after init. All conversion
 * functions are stateless and safe to call concurrently with the same coeffs.
 */

#ifndef RGB2YUV_H
#define RGB2YUV_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/*  Enums                                                                   */
/* ------------------------------------------------------------------------ */

typedef enum {
    RGB2YUV_BT601  = 0, /* ITU-R BT.601  (SDTV): Kr=0.299,  Kb=0.114  */
    RGB2YUV_BT709  = 1, /* ITU-R BT.709  (HDTV): Kr=0.2126, Kb=0.0722 */
    RGB2YUV_BT2020 = 2  /* ITU-R BT.2020 (UHDTV): Kr=0.2627, Kb=0.0593 */
} rgb2yuv_standard;

typedef enum {
    RGB2YUV_RANGE_FULL   = 0, /* Y: [0,255],  CbCr: [0,255]   (JFIF/JPEG) */
    RGB2YUV_RANGE_STUDIO = 1  /* Y: [16,235], CbCr: [16,240]  (broadcast) */
} rgb2yuv_range;

typedef enum {
    RGB2YUV_OK         =  0,
    RGB2YUV_ERR_NULL   = -1, /* NULL pointer argument                     */
    RGB2YUV_ERR_DIMS   = -2, /* Invalid dimensions (zero, negative, overflow) */
    RGB2YUV_ERR_STRIDE = -3  /* Stride too small for given width          */
} rgb2yuv_error;

/* ------------------------------------------------------------------------ */
/*  Coefficient structure                                                   */
/* ------------------------------------------------------------------------ */

/*
 * Pre-computed fixed-point coefficients for RGB→YCbCr conversion.
 * Initialize once with rgb2yuv_init(), reuse for multiple conversions.
 *
 * Internal layout (16-bit fixed-point, shift = 16):
 *   Y  = (yr*R + yg*G + yb*B + y_add) >> 16
 *   Cb = clamp( (cbr*R + cbg*G + cbb*B + c_add) >> 16 )
 *   Cr = clamp( (crr*R + crg*G + crb*B + c_add) >> 16 )
 *
 * Constraints enforced by init:
 *   yr + yg + yb  = Y_SCALE  (65536 for full, ~56284 for studio)
 *   cbr + cbg + cbb = 0
 *   crr + crg + crb = 0
 */
typedef struct {
    int32_t yr, yg, yb;    /* Y  row coefficients */
    int32_t cbr, cbg, cbb;  /* Cb row coefficients */
    int32_t crr, crg, crb;  /* Cr row coefficients */
    int32_t y_add;           /* Y  rounding + offset: y_bias * 65536 + 32768 */
    int32_t c_add;           /* CbCr rounding + offset: 128 * 65536 + 32768  */
} rgb2yuv_coeffs;

/* ------------------------------------------------------------------------ */
/*  API                                                                     */
/* ------------------------------------------------------------------------ */

/*
 * Initialize coefficients for a given standard and range.
 * Must be called before any conversion function.
 *
 * The coefficients are derived at runtime from the standard's (Kr, Kb) pair
 * using double-precision arithmetic, then quantized to 16-bit fixed-point
 * with algebraic constraints enforced (no accumulated rounding drift).
 */
void rgb2yuv_init(rgb2yuv_coeffs *coeffs,
                  rgb2yuv_standard standard,
                  rgb2yuv_range range);

/*
 * Convert packed RGB888 to YUV444 planar.
 *
 * Input:  rgb[height][rgb_stride] - packed R,G,B bytes (3 bytes per pixel)
 * Output: y[height][y_stride], u[height][u_stride], v[height][v_stride]
 *
 * All output planes have the same dimensions as input (no subsampling).
 * Returns RGB2YUV_OK on success, negative error code on failure.
 */
int rgb2yuv_444(const rgb2yuv_coeffs *coeffs,
                const uint8_t * restrict rgb, int rgb_stride,
                uint8_t * restrict y, int y_stride,
                uint8_t * restrict u, int u_stride,
                uint8_t * restrict v, int v_stride,
                int width, int height);

/*
 * Convert packed RGB888 to I420 planar (4:2:0 chroma subsampling).
 *
 * Input:  rgb[height][rgb_stride] - packed R,G,B bytes
 * Output: y[height][y_stride]     - full resolution luma
 *         u[ch][u_stride]         - half resolution Cb (ch = (height+1)/2)
 *         v[ch][v_stride]         - half resolution Cr
 *
 * Chroma planes are ceil(width/2) x ceil(height/2).
 * Chroma is computed by box-filtering (averaging) each 2x2 luma block
 * (MPEG-1 center-aligned siting). Odd dimensions handled correctly.
 *
 * Uses single-pass 2-row stripe processing: each RGB pixel is read exactly
 * once. The sum-then-convert approach avoids double rounding.
 */
int rgb2yuv_i420(const rgb2yuv_coeffs *coeffs,
                 const uint8_t * restrict rgb, int rgb_stride,
                 uint8_t * restrict y, int y_stride,
                 uint8_t * restrict u, int u_stride,
                 uint8_t * restrict v, int v_stride,
                 int width, int height);

/*
 * Convert packed RGB888 to NV12 semi-planar (4:2:0 chroma subsampling).
 *
 * Input:  rgb[height][rgb_stride] - packed R,G,B bytes
 * Output: y[height][y_stride]     - full resolution luma
 *         uv[ch][uv_stride]       - interleaved U,V pairs (ch = (height+1)/2)
 *
 * UV plane contains alternating Cb,Cr bytes: U0,V0,U1,V1,...
 * uv_stride >= 2 * ceil(width/2) = width + (width & 1).
 *
 * Same algorithmic approach as I420 (single-pass, sum-then-convert).
 */
int rgb2yuv_nv12(const rgb2yuv_coeffs *coeffs,
                 const uint8_t * restrict rgb, int rgb_stride,
                 uint8_t * restrict y, int y_stride,
                 uint8_t * restrict uv, int uv_stride,
                 int width, int height);

/*
 * Compute chroma plane dimensions for 4:2:0 formats (I420, NV12).
 * cw = (width + 1) / 2,  ch = (height + 1) / 2.
 * Either output pointer may be NULL if not needed.
 */
void rgb2yuv_chroma_size(int width, int height, int *cw, int *ch);

#ifdef __cplusplus
}
#endif

#endif /* RGB2YUV_H */
