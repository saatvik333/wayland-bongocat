#!/bin/bash

OUTPUT_DIR="$1"
HEADER_FILE="$2"
PREFIX="$3"

# Either (COLS and ROWS) or --frame-size must be provided
if [[ -z "$OUTPUT_DIR" || -z "$HEADER_FILE" || -z $PREFIX ]]; then
    echo "Usage: $0 <input-dir> <og-input-dir> <output-header> <output-source>"
    exit 1
fi

PREFIX_LOWER=$(echo "$PREFIX" | tr '[:upper:]' '[:lower:]')
PREFIX_UPPER=$(echo "$PREFIX" | tr '[:lower:]' '[:upper:]')


OUTPUT_FILE_1="${OUTPUT_DIR}/${PREFIX_LOWER}_init_digimon_anim.c.inl"
OUTPUT_FILE_2="${OUTPUT_DIR}/${PREFIX_LOWER}_init_digimon_embedded_images.c.inl"
OUTPUT_FILE_3="${OUTPUT_DIR}/${PREFIX_LOWER}_config_parse_enum_key.c.inl"

# Clean output files at the start
> "$OUTPUT_FILE_1"
> "$OUTPUT_FILE_2"
> "$OUTPUT_FILE_3"

# Extract lowercase digimon names based on the 'extern const unsigned char ..._*_png[];' pattern
grep -oP "extern const unsigned char ${PREFIX_LOWER}_\K[a-z0-9_]+(?=_png)" "$HEADER_FILE" | while read -r name; do
    upper_name=$(echo "$name" | tr '[:lower:]' '[:upper:]')
    lower_name=$(echo "$name" | tr '[:upper:]' '[:lower:]')

    echo "init_digimon_anim(ctx, ${PREFIX_UPPER}_${upper_name}_ANIM_INDEX, &digimon_sprite_sheet_embedded_images[${PREFIX_UPPER}_${upper_name}_ANIM_INDEX], ${PREFIX_UPPER}_${upper_name}_SPRITE_SHEET_COLS, ${PREFIX_UPPER}_${upper_name}_SPRITE_SHEET_ROWS);" >> "$OUTPUT_FILE_1"

    echo "digimon_sprite_sheet_embedded_images[${PREFIX_UPPER}_${upper_name}_ANIM_INDEX] = (embedded_image_t){${PREFIX_LOWER}_${lower_name}_png, ${PREFIX_LOWER}_${lower_name}_png_size, \"embedded ${name}\"};" >> "$OUTPUT_FILE_2"

    echo "if (strcmp(lower_value, \"${lower_name}\") == 0) {" >> "$OUTPUT_FILE_3"
    echo "    config->animation_index = ${PREFIX_UPPER}_${upper_name}_ANIM_INDEX;" >> "$OUTPUT_FILE_3"
    echo "}" >> "$OUTPUT_FILE_3"
done
