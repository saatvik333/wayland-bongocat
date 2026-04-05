#!/usr/bin/env bash
# Script to convert SVG assets to C header files for embedding.
# NOTE: This script should be run manually when assets change.
# The generated files are committed to git and not generated during build.

set -euo pipefail

ASSETS_DIR="assets/new"
OUTPUT_DIR="include/graphics"
OUTPUT_FILE="$OUTPUT_DIR/embedded_assets.h"
OUTPUT_C_FILE="src/graphics/embedded_assets.c"

mkdir -p "$OUTPUT_DIR" "src/graphics"

file_size_bytes() {
    local file="$1"
    if stat -c%s "$file" >/dev/null 2>&1; then
        stat -c%s "$file"
    else
        stat -f%z "$file"
    fi
}

echo "Generating embedded assets header..."

# Create header file
cat > "$OUTPUT_FILE" << 'EOF'
#ifndef EMBEDDED_ASSETS_H
#define EMBEDDED_ASSETS_H

#include <stddef.h>

// Embedded SVG asset data
extern const unsigned char bongo_both_up_svg[];
extern const size_t bongo_both_up_svg_size;

extern const unsigned char bongo_left_down_svg[];
extern const size_t bongo_left_down_svg_size;

extern const unsigned char bongo_right_down_svg[];
extern const size_t bongo_right_down_svg_size;

extern const unsigned char bongo_both_down_svg[];
extern const size_t bongo_both_down_svg_size;

extern const unsigned char bongo_sleeping_svg[];
extern const size_t bongo_sleeping_svg_size;

#endif // EMBEDDED_ASSETS_H
EOF

# Create source file with embedded data
cat > "$OUTPUT_C_FILE" << 'EOF'
#include "graphics/embedded_assets.h"

EOF

# Temporary directory for preprocessed SVGs
TMP_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_DIR"' EXIT

# Convert each SVG to C array
for asset in "bongo-both-up.svg" "bongo-left-down.svg" "bongo-right-down.svg" "bongo-both-down.svg" "bongo-sleeping.svg"; do
    if [ -f "$ASSETS_DIR/$asset" ]; then
        echo "Embedding $asset..."

        # Preprocess: crop viewBox to content area, remove editor artifacts
        tmp_file="$TMP_DIR/$asset"
        sed -e 's/viewBox="0 0 500 500"/viewBox="0 101 500 277"/' \
            -e '/<rect /d' \
            "$ASSETS_DIR/$asset" > "$tmp_file"

        # Convert filename to C identifier (strip extension, replace non-alnum with _)
        c_name=$(echo "${asset%.svg}" | sed 's/[^a-zA-Z0-9]/_/g')_svg

        # Generate C array using xxd
        xxd -i "$tmp_file" | sed "s/unsigned char.*\[\]/const unsigned char ${c_name}[]/" >> "$OUTPUT_C_FILE"
        echo "" >> "$OUTPUT_C_FILE"

        # Add size variable
        size=$(file_size_bytes "$tmp_file")
        echo "const size_t ${c_name}_size = $size;" >> "$OUTPUT_C_FILE"
        echo "" >> "$OUTPUT_C_FILE"
    else
        echo "Warning: $ASSETS_DIR/$asset not found"
    fi
done

echo "Assets embedded successfully!"
echo "Generated: $OUTPUT_FILE"
echo "Generated: $OUTPUT_C_FILE"
