#!/bin/bash
#
# gen_reference.sh - Generate FFmpeg reference files for cross-validation
#
# Requires: ffmpeg
# Output: tests/ref/ directory with reference images and YUV files
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REF_DIR="$SCRIPT_DIR/ref"
mkdir -p "$REF_DIR"

if ! command -v ffmpeg &>/dev/null; then
    echo "ERROR: ffmpeg not found. Install ffmpeg to generate reference files."
    exit 1
fi

echo "Generating reference files in $REF_DIR ..."

# 1. SMPTE color bars (640x480)
echo "  SMPTE bars 640x480..."
ffmpeg -y -f lavfi -i "smptebars=size=640x480:duration=1" \
    -frames:v 1 "$REF_DIR/smpte_640x480.png" 2>/dev/null

ffmpeg -y -i "$REF_DIR/smpte_640x480.png" \
    -pix_fmt yuv420p -f rawvideo "$REF_DIR/smpte_640x480_bt601_studio.i420" 2>/dev/null

ffmpeg -y -i "$REF_DIR/smpte_640x480.png" \
    -pix_fmt nv12 -f rawvideo "$REF_DIR/smpte_640x480_bt601_studio.nv12" 2>/dev/null

ffmpeg -y -i "$REF_DIR/smpte_640x480.png" \
    -pix_fmt yuv444p -f rawvideo "$REF_DIR/smpte_640x480_bt601_studio.yuv444" 2>/dev/null

# 2. Small test patterns for odd dimensions
echo "  Color gradient 7x5..."
ffmpeg -y -f lavfi -i "testsrc2=size=7x5:duration=1" \
    -frames:v 1 "$REF_DIR/gradient_7x5.png" 2>/dev/null

ffmpeg -y -i "$REF_DIR/gradient_7x5.png" \
    -pix_fmt yuv420p -f rawvideo "$REF_DIR/gradient_7x5_bt601_studio.i420" 2>/dev/null

# 3. Pure colors 2x2 for exact verification (1x1 not supported by all ffmpeg builds)
for color in "red=FF0000" "green=00FF00" "blue=0000FF" "white=FFFFFF" "black=000000"; do
    name="${color%%=*}"
    hex="${color##*=}"
    echo "  2x2 $name..."
    ffmpeg -y -f lavfi -i "color=c=0x${hex}:size=2x2:duration=1" \
        -frames:v 1 "$REF_DIR/${name}_2x2.png" 2>/dev/null || \
    echo "  WARNING: failed to generate $name (ffmpeg issue)"
done

echo ""
echo "Reference files generated:"
ls -la "$REF_DIR/"
echo ""
echo "To compare with rgb2yuv output:"
echo "  ./build/rgb2yuv -s bt601 -r studio -f i420 tests/ref/smpte_640x480.png /tmp/our.i420"
echo "  cmp tests/ref/smpte_640x480_bt601_studio.i420 /tmp/our.i420"
