#!/bin/bash

# Default padding
PAD_TOP=0
PAD_RIGHT=0
PAD_BOTTOM=0
PAD_LEFT=0
FRAME_SIZE=""

# === Required args ===
INPUT="$1"
OUTPUT="$2"
COLS=""
ROWS=""

# === Parse args ===
POSITIONAL_ARGS=()
while [[ $# -gt 0 ]]; do
    case $1 in
        --pad-top) PAD_TOP="$2"; shift 2 ;;
        --pad-right) PAD_RIGHT="$2"; shift 2 ;;
        --pad-bottom) PAD_BOTTOM="$2"; shift 2 ;;
        --pad-left) PAD_LEFT="$2"; shift 2 ;;
        --frame-size) FRAME_SIZE="$2"; shift 2 ;;
        --cols) COLS="$2"; shift 2 ;;
        --rows) ROWS="$2"; shift 2 ;;
        --padding) PAD_TOP="$2";PAD_RIGHT="$2";PAD_BOTTOM="$2";PAD_LEFT="$2"; shift 2 ;;
        -*|--*)
            echo "Unknown option $1"; exit 1 ;;
        *) POSITIONAL_ARGS+=("$1"); shift ;;
    esac
done
set -- "${POSITIONAL_ARGS[@]}"

INPUT="${POSITIONAL_ARGS[0]}"
OUTPUT="${POSITIONAL_ARGS[1]}"

# === Dependency check ===
if ! command -v magick &>/dev/null; then
    echo "ImageMagick v7+ (magick command) is required."
    exit 1
fi

# Handle optional frame size or COLS/ROWS
if [[ -n "$FRAME_SIZE" ]]; then
  SHEET_WIDTH=$(magick identify -format "%w" "$INPUT")
  SHEET_HEIGHT=$(magick identify -format "%h" "$INPUT")

  COLS=$(( SHEET_WIDTH / FRAME_SIZE ))
  ROWS=$(( SHEET_HEIGHT / FRAME_SIZE ))
fi

# Either (COLS and ROWS) or --frame-size must be provided
if [[ -z "$INPUT" || -z "$OUTPUT" || -z "$COLS" || -z "$ROWS" ]]; then
  echo "Usage: $0 input.png output.png [--cols N --rows N] [--frame-size N] [--padding N] [--pad-* N]"
  exit 1
fi


# === Setup ===
WORKDIR=$(mktemp -d)
trap 'rm -rf "$WORKDIR"' EXIT

# === Frame size ===
FRAME_WIDTH=$(magick identify -format "%[fx:w/$COLS]" "$INPUT")
FRAME_HEIGHT=$(magick identify -format "%[fx:h/$ROWS]" "$INPUT")
echo "Splitting into ${COLS}x${ROWS} of ${FRAME_WIDTH}x${FRAME_HEIGHT} frames..."

# === Step 1: Split into individual frames ===
magick "$INPUT" -crop "${FRAME_WIDTH}x${FRAME_HEIGHT}" +repage +adjoin PNG:"$WORKDIR/frame.png"

# === Step 2: Trim frames and record bounding boxes ===
MAX_W=0
MAX_H=0
MIN_TOP=99999  # track highest pixel

N=0
for f in "$WORKDIR"/frame-*.png; do
    NN=$(printf "%04d" "$N")
    OUT="$WORKDIR/trimmed-frame-$NN.png"
    magick "$f" -trim +repage "$OUT"

    read -r W H < <(magick identify -format "%w %h" "$OUT")
    (( W > MAX_W )) && MAX_W=$W
    (( H > MAX_H )) && MAX_H=$H

    # Get top offset (distance from top of original frame to trimmed image)
    Y_OFFSET=$(magick identify -format "%[fx:page.y]" "$OUT")
    (( Y_OFFSET < MIN_TOP )) && MIN_TOP=$Y_OFFSET


    echo "Processing $f ->$OUT"
    echo "Dimensions: $W x $H, Y offset: $Y_OFFSET"

    ((N++))
done

echo "Max content size (before padding): ${MAX_W}x${MAX_H}"
echo "Topmost visible pixel across all frames starts at Y=$MIN_TOP"

# Adjust height to remove vertical top space above MIN_TOP
NEW_FRAME_HEIGHT=$((MAX_H + PAD_TOP + PAD_BOTTOM))
NEW_FRAME_WIDTH=$((MAX_W + PAD_LEFT + PAD_RIGHT))

echo "New unified frame size (after cropping + padding): ${NEW_FRAME_WIDTH}x${NEW_FRAME_HEIGHT}"

if (( MAX_H <= MIN_TOP )); then
    echo "Error: MIN_TOP ($MIN_TOP) is greater than or equal to MAX_H ($MAX_H)"
    exit 1
fi

# === Step 3: Create padded, bottom-aligned frames ===
N=0
for f in "$WORKDIR"/trimmed-frame-*.png; do
    NN=$(printf "%04d" "$N")
    PADDED="$WORKDIR/padded_$NN.png"

    echo "Generating padded frame: $PADDED"
    echo "  MIN_TOP=$MIN_TOP  MAX_W=$MAX_W  MAX_H=$MAX_H"
    echo "  Extent: ${MAX_W}x$((MAX_H - MIN_TOP))"
    echo "  border: ${PAD_LEFT}x${PAD_BOTTOM}"
    echo "  border: ${PAD_RIGHT}x${PAD_TOP}"

    # Re-trim (ensure trimmed version), then remove excess top and pad
    # Shift up by MIN_TOP (i.e. crop that much off the top), then pad
    magick "$f" \
        -gravity north -chop 0x${MIN_TOP} +repage \
        -gravity south \
        -background none \
        -extent "${MAX_W}x$((MAX_H - MIN_TOP))" \
        -bordercolor none \
        -border "${PAD_LEFT}x${PAD_BOTTOM}" \
        -gravity north \
        -border "${PAD_RIGHT}x${PAD_TOP}" \
        "$PADDED" || echo "Warning: Failed to generate $PADDED"

    echo magick "$f" -gravity north -chop 0x${MIN_TOP} +repage -gravity south -background none -extent "${MAX_W}x$((MAX_H - MIN_TOP))" -bordercolor none -border "${PAD_LEFT}x${PAD_BOTTOM}" -gravity north -border "${PAD_RIGHT}x${PAD_TOP}" "$PADDED"

    ((N++))
done

# === Step 4: Reassemble sprite sheet ===
echo "Reassembling sprite sheet to: $OUTPUT"
magick montage "$WORKDIR"/padded_*.png -tile "${COLS}x${ROWS}" -geometry +0+0 -background none "$OUTPUT" || echo "Warning: Failed to generate $OUTPUT"

echo "magick montage ${WORKDIR}/padded_*.png -tile ${COLS}x${ROWS} -geometry +0+0 -background none ${OUTPUT}"

echo "Output path: $OUTPUT"