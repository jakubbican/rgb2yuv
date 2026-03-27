/*
 * main.c - RGB to YUV CLI converter
 *
 * Usage: rgb2yuv [options] <input> <output>
 *
 * Reads PNG/JPEG/BMP/TGA via stb_image, converts to YUV, writes raw output.
 */

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "rgb2yuv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------ */
/*  Usage / help                                                            */
/* ------------------------------------------------------------------------ */

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options] <input> <output>\n"
        "\n"
        "Convert RGB image to raw YUV.\n"
        "\n"
        "Options:\n"
        "  -s, --standard <bt601|bt709|bt2020>  Color standard (default: bt601)\n"
        "  -r, --range <full|studio>            Value range (default: full)\n"
        "  -f, --format <yuv444|i420|nv12>      Output format (default: i420)\n"
        "  --info                               Print conversion info and exit\n"
        "  -h, --help                           Show this help\n"
        "\n"
        "Input:  PNG, JPEG, BMP, TGA (any format supported by stb_image)\n"
        "Output: Raw YUV data (Y plane, then U/V or UV plane)\n",
        prog);
}

/* ------------------------------------------------------------------------ */
/*  Argument parsing                                                        */
/* ------------------------------------------------------------------------ */

typedef struct {
    const char *input;
    const char *output;
    rgb2yuv_standard standard;
    rgb2yuv_range range;
    int format;   /* 0=i420, 1=nv12, 2=yuv444 */
    int info_only;
} cli_args;

static int match_arg(const char *arg, const char *short_opt, const char *long_opt)
{
    return (strcmp(arg, short_opt) == 0) || (strcmp(arg, long_opt) == 0);
}

static int parse_args(int argc, char *argv[], cli_args *args)
{
    memset(args, 0, sizeof(*args));
    args->standard = RGB2YUV_BT601;
    args->range = RGB2YUV_RANGE_FULL;
    args->format = 0; /* i420 */

    int positional = 0;

    for (int i = 1; i < argc; i++) {
        if (match_arg(argv[i], "-h", "--help")) {
            print_usage(argv[0]);
            return -1;
        }
        if (strcmp(argv[i], "--info") == 0) {
            args->info_only = 1;
            continue;
        }
        if (match_arg(argv[i], "-s", "--standard")) {
            if (++i >= argc) { fprintf(stderr, "Missing value for -s\n"); return -1; }
            if (strcmp(argv[i], "bt601") == 0) args->standard = RGB2YUV_BT601;
            else if (strcmp(argv[i], "bt709") == 0) args->standard = RGB2YUV_BT709;
            else if (strcmp(argv[i], "bt2020") == 0) args->standard = RGB2YUV_BT2020;
            else { fprintf(stderr, "Unknown standard: %s\n", argv[i]); return -1; }
            continue;
        }
        if (match_arg(argv[i], "-r", "--range")) {
            if (++i >= argc) { fprintf(stderr, "Missing value for -r\n"); return -1; }
            if (strcmp(argv[i], "full") == 0) args->range = RGB2YUV_RANGE_FULL;
            else if (strcmp(argv[i], "studio") == 0) args->range = RGB2YUV_RANGE_STUDIO;
            else { fprintf(stderr, "Unknown range: %s\n", argv[i]); return -1; }
            continue;
        }
        if (match_arg(argv[i], "-f", "--format")) {
            if (++i >= argc) { fprintf(stderr, "Missing value for -f\n"); return -1; }
            if (strcmp(argv[i], "i420") == 0) args->format = 0;
            else if (strcmp(argv[i], "nv12") == 0) args->format = 1;
            else if (strcmp(argv[i], "yuv444") == 0) args->format = 2;
            else { fprintf(stderr, "Unknown format: %s\n", argv[i]); return -1; }
            continue;
        }
        if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return -1;
        }
        /* Positional */
        if (positional == 0) args->input = argv[i];
        else if (positional == 1) args->output = argv[i];
        else { fprintf(stderr, "Too many arguments\n"); return -1; }
        positional++;
    }

    if (!args->info_only && positional < 2) {
        fprintf(stderr, "Missing input or output file\n");
        print_usage(argv[0]);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/*  Info printing                                                           */
/* ------------------------------------------------------------------------ */

static const char *std_name(rgb2yuv_standard s)
{
    switch (s) {
        case RGB2YUV_BT601:  return "BT.601";
        case RGB2YUV_BT709:  return "BT.709";
        case RGB2YUV_BT2020: return "BT.2020";
    }
    return "unknown";
}

static const char *range_name(rgb2yuv_range r)
{
    return r == RGB2YUV_RANGE_STUDIO ? "studio" : "full";
}

static const char *format_name(int f)
{
    switch (f) {
        case 0: return "I420 (planar 4:2:0)";
        case 1: return "NV12 (semi-planar 4:2:0)";
        case 2: return "YUV444 (planar)";
    }
    return "unknown";
}

static void print_info(const cli_args *args, const rgb2yuv_coeffs *c)
{
    printf("Conversion parameters:\n");
    printf("  Standard:     %s\n", std_name(args->standard));
    printf("  Range:        %s\n", range_name(args->range));
    printf("  Format:       %s\n", format_name(args->format));
    printf("  Coefficients (fixed-point, <<16):\n");
    printf("    Y:  [%6d, %6d, %6d]  sum=%d\n",
           c->yr, c->yg, c->yb, c->yr + c->yg + c->yb);
    printf("    Cb: [%6d, %6d, %6d]  sum=%d\n",
           c->cbr, c->cbg, c->cbb, c->cbr + c->cbg + c->cbb);
    printf("    Cr: [%6d, %6d, %6d]  sum=%d\n",
           c->crr, c->crg, c->crb, c->crr + c->crg + c->crb);
    printf("    y_add=%d  c_add=%d\n", c->y_add, c->c_add);
}

/* ------------------------------------------------------------------------ */
/*  Main                                                                    */
/* ------------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    cli_args args;
    if (parse_args(argc, argv, &args) != 0)
        return 1;

    rgb2yuv_coeffs coeffs;
    rgb2yuv_init(&coeffs, args.standard, args.range);

    if (args.info_only) {
        print_info(&args, &coeffs);
        return 0;
    }

    /* Load image */
    int width, height, channels;
    uint8_t *rgb = stbi_load(args.input, &width, &height, &channels, 3);
    if (!rgb) {
        fprintf(stderr, "Failed to load image: %s\n", args.input);
        return 1;
    }

    printf("Input:  %s (%dx%d, %d channels)\n", args.input, width, height, channels);
    printf("Output: %s (%s, %s, %s)\n", args.output,
           format_name(args.format), std_name(args.standard), range_name(args.range));

    /* Compute output size and allocate */
    int cw, ch;
    rgb2yuv_chroma_size(width, height, &cw, &ch);

    size_t y_size = (size_t)width * (size_t)height;
    size_t total_size;

    switch (args.format) {
        case 0: /* I420 */
            total_size = y_size + (size_t)cw * (size_t)ch * 2;
            break;
        case 1: /* NV12 */
            total_size = y_size + (size_t)cw * 2 * (size_t)ch;
            break;
        case 2: /* YUV444 */
            total_size = y_size * 3;
            break;
        default:
            total_size = 0;
            break;
    }

    uint8_t *out = calloc(total_size, 1);
    if (!out) {
        fprintf(stderr, "Failed to allocate %zu bytes for output\n", total_size);
        stbi_image_free(rgb);
        return 1;
    }

    /* Convert */
    int rc;
    uint8_t *y_plane = out;

    switch (args.format) {
        case 0: { /* I420 */
            uint8_t *u_plane = y_plane + y_size;
            uint8_t *v_plane = u_plane + (size_t)cw * (size_t)ch;
            rc = rgb2yuv_i420(&coeffs, rgb, width * 3,
                              y_plane, width, u_plane, cw, v_plane, cw,
                              width, height);
            break;
        }
        case 1: { /* NV12 */
            uint8_t *uv_plane = y_plane + y_size;
            rc = rgb2yuv_nv12(&coeffs, rgb, width * 3,
                              y_plane, width, uv_plane, cw * 2,
                              width, height);
            break;
        }
        case 2: { /* YUV444 */
            uint8_t *u_plane = y_plane + y_size;
            uint8_t *v_plane = u_plane + y_size;
            rc = rgb2yuv_444(&coeffs, rgb, width * 3,
                             y_plane, width, u_plane, width, v_plane, width,
                             width, height);
            break;
        }
        default:
            rc = -1;
            break;
    }

    stbi_image_free(rgb);

    if (rc != RGB2YUV_OK) {
        fprintf(stderr, "Conversion failed with error %d\n", rc);
        free(out);
        return 1;
    }

    /* Write output */
    FILE *fp = fopen(args.output, "wb");
    if (!fp) {
        fprintf(stderr, "Failed to open output file: %s\n", args.output);
        free(out);
        return 1;
    }

    size_t written = fwrite(out, 1, total_size, fp);
    fclose(fp);
    free(out);

    if (written != total_size) {
        fprintf(stderr, "Write error: wrote %zu of %zu bytes\n", written, total_size);
        return 1;
    }

    printf("Written %zu bytes\n", total_size);
    return 0;
}
