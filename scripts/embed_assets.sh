#!/bin/bash

# Script to convert PNG assets to C header files for embedding

ASSETS_DIR="assets"
OUTPUT_DIR="include"
OUTPUT_FILE="$OUTPUT_DIR/embedded_assets.h"

echo "Generating embedded assets header..."

# Create header file
cat > "$OUTPUT_FILE" << 'EOF'
#ifndef EMBEDDED_ASSETS_H
#define EMBEDDED_ASSETS_H

#include <stddef.h>

// Embedded asset data
extern const unsigned char bongo_cat_both_up_png[];
extern const size_t bongo_cat_both_up_png_size;

extern const unsigned char bongo_cat_left_down_png[];
extern const size_t bongo_cat_left_down_png_size;

extern const unsigned char bongo_cat_right_down_png[];
extern const size_t bongo_cat_right_down_png_size;

#endif // EMBEDDED_ASSETS_H
EOF

# Create source file with embedded data
OUTPUT_C_FILE="src/embedded_assets.c"

cat > "$OUTPUT_C_FILE" << 'EOF'
#include "embedded_assets.h"

EOF

# Convert each PNG to C array
for asset in "bongo-cat-both-up.png" "bongo-cat-left-down.png" "bongo-cat-right-down.png"; do
    if [ -f "$ASSETS_DIR/$asset" ]; then
        echo "Embedding $asset..."
        
        # Convert filename to C identifier
        c_name=$(echo "$asset" | sed 's/[^a-zA-Z0-9]/_/g')
        
        # Generate C array using xxd
        xxd -i "$ASSETS_DIR/$asset" | sed "s/unsigned char.*\[\]/const unsigned char ${c_name}[]/" >> "$OUTPUT_C_FILE"
        echo "" >> "$OUTPUT_C_FILE"
        
        # Add size variable
        size=$(stat -c%s "$ASSETS_DIR/$asset")
        echo "const size_t ${c_name}_size = $size;" >> "$OUTPUT_C_FILE"
        echo "" >> "$OUTPUT_C_FILE"
    else
        echo "Warning: $ASSETS_DIR/$asset not found"
    fi
done

echo "Assets embedded successfully!"
echo "Generated: $OUTPUT_FILE"
echo "Generated: $OUTPUT_C_FILE"