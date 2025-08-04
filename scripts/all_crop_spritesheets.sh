#!/bin/bash

# === Configuration ===
SCRIPT="./scripts/crop_spritesheet.sh"
FRAME_SIZE=128
PADDING=4


# === Iterate over PNGs ===
INPUT_DIR="assets/input/dm"
OUTPUT_DIR="assets/dm"
for FILE in "$INPUT_DIR"/*.png; do
    BASENAME=$(basename "$FILE")
    echo "Processing: $BASENAME"

    "$SCRIPT" "$FILE" "$OUTPUT_DIR/$BASENAME" "$BOTTOM_PADDING" --frame-size "$FRAME_SIZE" --padding "$PADDING"

    if [[ $? -ne 0 ]]; then
        echo "Failed to process $FILE"
    else
        echo "Finished $FILE"
    fi
done


INPUT_DIR="assets/input/dm20"
OUTPUT_DIR="assets/dm20"
for FILE in "$INPUT_DIR"/*.png; do
    BASENAME=$(basename "$FILE")
    echo "Processing: $BASENAME"

    "$SCRIPT" "$FILE" "$OUTPUT_DIR/$BASENAME" "$BOTTOM_PADDING" --frame-size "$FRAME_SIZE" --padding "$PADDING"

    if [[ $? -ne 0 ]]; then
        echo "Failed to process $FILE"
    else
        echo "Finished $FILE"
    fi
done


INPUT_DIR="assets/input/dmc"
OUTPUT_DIR="assets/dmc"
for FILE in "$INPUT_DIR"/*.png; do
    BASENAME=$(basename "$FILE")
    echo "Processing: $BASENAME"

    "$SCRIPT" "$FILE" "$OUTPUT_DIR/$BASENAME" "$BOTTOM_PADDING" --frame-size "$FRAME_SIZE" --padding "$PADDING"

    if [[ $? -ne 0 ]]; then
        echo "Failed to process $FILE"
    else
        echo "Finished $FILE"
    fi
done


INPUT_DIR="assets/input/dmx"
OUTPUT_DIR="assets/dmx"
for FILE in "$INPUT_DIR"/*.png; do
    BASENAME=$(basename "$FILE")
    echo "Processing: $BASENAME"

    "$SCRIPT" "$FILE" "$OUTPUT_DIR/$BASENAME" "$BOTTOM_PADDING" --frame-size "$FRAME_SIZE" --padding "$PADDING"

    if [[ $? -ne 0 ]]; then
        echo "Failed to process $FILE"
    else
        echo "Finished $FILE"
    fi
done


INPUT_DIR="assets/input/pen20"
OUTPUT_DIR="assets/pen20"
for FILE in "$INPUT_DIR"/*.png; do
    BASENAME=$(basename "$FILE")
    echo "Processing: $BASENAME"

    "$SCRIPT" "$FILE" "$OUTPUT_DIR/$BASENAME" "$BOTTOM_PADDING" --frame-size "$FRAME_SIZE" --padding "$PADDING"

    if [[ $? -ne 0 ]]; then
        echo "Failed to process $FILE"
    else
        echo "Finished $FILE"
    fi
done

echo "All done!"
