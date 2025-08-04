#!/bin/bash

# === Usage Check ===
if [[ $# -lt 3 ]]; then
    echo "Usage: $0 <input-dir> <og-input-dir> <output-header> <output-source>"
    echo "Example: $0 assets/dm20 assets/input/dm20 include/graphics/embedded_assets/dm20.h src/graphics/dm20.c"
    exit 1
fi

# === Arguments ===
INPUT_DIR="$1"
OG_INPUT_DIR="$2"
HEADER_OUT="$3"
SOURCE_OUT="$4"
FRAME_SIZE=""
COLS=""
ROWS=""

# === Parse args ===
POSITIONAL_ARGS=()
while [[ $# -gt 0 ]]; do
    case $1 in
        --frame-size) FRAME_SIZE="$2"; shift 2 ;;
        --cols) COLS="$2"; shift 2 ;;
        --rows) ROWS="$2"; shift 2 ;;
        -*|--*)
            echo "Unknown option $1"; exit 1 ;;
        *) POSITIONAL_ARGS+=("$1"); shift ;;
    esac
done
set -- "${POSITIONAL_ARGS[@]}"

INPUT="${POSITIONAL_ARGS[0]}"
OG_INPUT="${POSITIONAL_ARGS[1]}"
OUTPUT="${POSITIONAL_ARGS[2]}"

# === Dependency check ===
if ! command -v magick &>/dev/null; then
    echo "ImageMagick v7+ (magick command) is required."
    exit 1
fi

if [[ -z "$INPUT" || -z "$OG_INPUT" || -z "$OUTPUT" ]]; then
    echo "Usage: $0 <input-dir> <og-input-dir> <output-header> <output-source>"
    exit 1
fi


# === Derived prefix from directory (after 'assets/') ===
ASSETS_PREFIX=${INPUT_DIR#assets/}
ASSETS_PREFIX_CLEAN=$(echo "$ASSETS_PREFIX" | sed "s/['().:]//g")
ASSETS_PREFIX_CLEAN=$(echo "$ASSETS_PREFIX_CLEAN" | sed "s/.png$//g")
ASSETS_PREFIX_CLEAN=$(echo "$ASSETS_PREFIX_CLEAN" | sed 's/[^a-zA-Z0-9]/_/g')
ASSETS_PREFIX_CLEAN=$(echo "$ASSETS_PREFIX_CLEAN" | sed 's/_\+/_/g')
ASSETS_PREFIX_LOWER=$(echo "$ASSETS_PREFIX_CLEAN" | tr '[:upper:]' '[:lower:]')
ASSETS_PREFIX_UPPER=$(echo "$ASSETS_PREFIX_CLEAN" | tr '[:lower:]' '[:upper:]')

# Clean output files at the start
> "$HEADER_OUT"
> "$SOURCE_OUT"

# === Header file intro ===
HEADER_GUARD="${ASSETS_PREFIX_UPPER}_EMBEDDED_ASSETS_H"
echo "#ifndef $HEADER_GUARD" >> "$HEADER_OUT"
echo "#define $HEADER_GUARD" >> "$HEADER_OUT"
echo >> "$HEADER_OUT"
echo "#include <stddef.h>" >> "$HEADER_OUT"
echo >> "$HEADER_OUT"
echo "/// @NOTE: Generated embedded assets from $INPUT_DIR" >> "$HEADER_OUT"
echo >> "$HEADER_OUT"

# === Source file intro ===
HEADER_RELATIVE_PATH="${HEADER_OUT#include/}"
echo "#include \"$HEADER_RELATIVE_PATH\"" >> "$SOURCE_OUT"
echo >> "$SOURCE_OUT"
echo "/// @NOTE: Generated embedded assets from $INPUT_DIR" >> "$SOURCE_OUT"
echo >> "$SOURCE_OUT"

# === Start animation index counter ===
START_INDEX=1
INDEX=$START_INDEX

# === Process all PNGs ===
for FILE in "$INPUT_DIR"/*.png; do
    BASENAME=$(basename "$FILE")

    # Handle optional frame size or COLS/ROWS
    if [[ -n "$FRAME_SIZE" ]]; then
        OG_FILE="$OG_INPUT_DIR/$BASENAME"
        if [ -f "$OG_FILE" ]; then
            SHEET_WIDTH=$(magick identify -format "%w" "$OG_FILE")
            SHEET_HEIGHT=$(magick identify -format "%h" "$OG_FILE")

            COLS=$(( SHEET_WIDTH / FRAME_SIZE ))
            ROWS=$(( SHEET_HEIGHT / FRAME_SIZE ))
        else
            COLS=0
            ROWS=0
            echo "$OG_FILE not found"
            continue
        fi
    fi
    FRAMES_COUNT=$(( COLS * ROWS ))

    NAME_NO_EXT="${BASENAME%.png}"
    NAME_CLEAN=$(echo "$NAME_NO_EXT" | sed "s/['().:]//g")
    NAME_CLEAN=$(echo "$NAME_CLEAN" | sed 's/[^a-zA-Z0-9]/_/g')
    NAME_CLEAN=$(echo "$NAME_CLEAN" | sed 's/_\+/_/g')
    IDENTIFIER=$(echo "$NAME_CLEAN" | tr '[:upper:]' '[:lower:]')
    MACRO_PREFIX=$(echo "${ASSETS_PREFIX_UPPER}_${IDENTIFIER}" | tr '[:lower:]' '[:upper:]')

    EMBED_SYMBOL="${ASSETS_PREFIX_LOWER}_${IDENTIFIER}_png"
    SIZE_SYMBOL="${EMBED_SYMBOL}_size"
    RELATIVE_PATH="../../../$INPUT_DIR/$BASENAME"

    # === Header content ===
    echo "// Name: $NAME_NO_EXT" >> "$HEADER_OUT"
    echo "#define ${MACRO_PREFIX}_NAME \"$NAME_NO_EXT\"" >> "$HEADER_OUT"
    echo "extern const unsigned char $EMBED_SYMBOL[];" >> "$HEADER_OUT"
    echo "extern const size_t $SIZE_SYMBOL;" >> "$HEADER_OUT"
    echo "#define ${MACRO_PREFIX}_SPRITE_SHEET_COLS $COLS" >> "$HEADER_OUT"
    echo "#define ${MACRO_PREFIX}_SPRITE_SHEET_ROWS $ROWS" >> "$HEADER_OUT"
    echo "#define ${MACRO_PREFIX}_SPRITE_SHEET_FRAMES_COUNT $FRAMES_COUNT" >> "$HEADER_OUT"
    echo "#define ${MACRO_PREFIX}_ANIM_INDEX $INDEX" >> "$HEADER_OUT"
    echo >> "$HEADER_OUT"

    # === Source content ===
    echo "// Name: $NAME_NO_EXT" >> "$SOURCE_OUT"
    echo "const unsigned char $EMBED_SYMBOL[] = {" >> "$SOURCE_OUT"
    echo "#embed \"$RELATIVE_PATH\"" >> "$SOURCE_OUT"
    echo "};" >> "$SOURCE_OUT"
    echo "const size_t $SIZE_SYMBOL = sizeof($EMBED_SYMBOL);" >> "$SOURCE_OUT"
    echo >> "$SOURCE_OUT"

    ((INDEX++))
done


echo "#define ${ASSETS_PREFIX_UPPER}_ANIM_COUNT $((INDEX-START_INDEX))" >> "$HEADER_OUT"
echo >> "$HEADER_OUT"

echo "#endif // $HEADER_GUARD" >> "$HEADER_OUT"