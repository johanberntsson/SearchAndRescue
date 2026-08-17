#!/usr/bin/env python3
"""
C64 Binary Font (.fnt/.prg) to PNG Grid Converter

This script reads a raw Commodore 64 font file and exports it as a clean
PNG image grid. Each character cell is 8x8 pixels. The default grid is
32 characters wide by 8 characters deep, covering all 256 possible PETSCII/ROM glyphs.

C64 Font File Structure:
- Raw binary data.
- Standard set: 2048 bytes (256 characters * 8 bytes per character).
- Some files contain a 2-byte loader header (usually $00 $38 or similar).
- This script automatically detects and skips the 2-byte header if present.

Usage:
    python c64font_to_png.py <input_font_file.fnt> [<output_image.png>]

Example:
    python c64font_to_png.py uppercase.fnt output_grid.png
"""

import sys
import os
from PIL import Image

# --- Configuration Section ---

# Dimensions of a standard C64 character cell
CHAR_WIDTH = 8
CHAR_HEIGHT = 8

# Layout of the final PNG grid (Must multiply to 256)
GRID_CHARS_X = 32  # Number of characters across
GRID_CHARS_Y = 8   # Number of characters down

# Colors: C64 Classic Blue/Cyan Theme
# Color format is (Red, Green, Blue) from 0-255
COLOR_BACKGROUND = (64, 64, 224) # Dark Blue
COLOR_FOREGROUND = (160, 255, 255) # Light Cyan

# Color format: (White on Black)
# COLOR_BACKGROUND = (0, 0, 0)
# COLOR_FOREGROUND = (255, 255, 255)

# -----------------------------

def convert_font_to_png(input_path, output_path=None):
    """Reads C64 font binary and saves it as a PNG grid."""
    
    # Define output filename if not provided
    if not output_path:
        base, _ = os.path.splitext(input_path)
        output_path = f"{base}_grid.png"

    # 1. Load the raw binary data
    try:
        with open(input_path, 'rb') as f:
            font_data = f.read()
    except FileNotFoundError:
        print(f"Error: File not found at '{input_path}'")
        sys.exit(1)

    print(f"Loaded '{input_path}': {len(font_data)} bytes.")

    # 2. Handle potential C64 loader header (2 bytes)
    # A standard font set is 2048 bytes. A .prg version might be 2050.
    if len(font_data) % 2048 == 2:
        print("Detected 2-byte C64 header. Skipping.")
        font_data = font_data[2:]
    
    if len(font_data) < 2048:
        print(f"Error: Font data must be at least 2048 bytes (raw). "
              f"Got {len(font_data)} bytes.")
        sys.exit(1)

    # 3. Calculate canvas size
    canvas_width = GRID_CHARS_X * CHAR_WIDTH
    canvas_height = GRID_CHARS_Y * CHAR_HEIGHT

    # 4. Create new Pillow image canvas (RGB mode)
    image = Image.new('RGB', (canvas_width, canvas_height), COLOR_BACKGROUND)
    pixels = image.load() # Get pixel map for fast writing

    # 5. Process all 256 characters
    print(f"Generating {GRID_CHARS_X}x{GRID_CHARS_Y} grid "
          f"({canvas_width}x{canvas_height} pixels)...")

    for char_code in range(256):
        # Find this character's start index in the data
        char_data_start = char_code * CHAR_HEIGHT
        
        # Calculate where this character cell begins on the grid
        grid_x = char_code % GRID_CHARS_X
        grid_y = char_code // GRID_CHARS_X
        
        pixel_base_x = grid_x * CHAR_WIDTH
        pixel_base_y = grid_y * CHAR_HEIGHT

        # Process the 8 rows of the character bitmap
        for row_idx in range(CHAR_HEIGHT):
            try:
                row_byte = font_data[char_data_start + row_idx]
            except IndexError:
                break # In case of truncated font file

            # Each row byte contains 8 bits, where bit 7 is the leftmost pixel
            for col_idx in range(CHAR_WIDTH):
                # Check if the bit at the current column is set (foreground)
                # Bit shifting: Move relevant bit to position 0 and AND with 1
                if (row_byte >> (7 - col_idx)) & 1:
                    # Paint the pixel with the foreground color
                    current_x = pixel_base_x + col_idx
                    current_y = pixel_base_y + row_idx
                    
                    # Safety check to stay within canvas bounds
                    if current_x < canvas_width and current_y < canvas_height:
                        pixels[current_x, current_y] = COLOR_FOREGROUND

    # 6. Save the final image
    try:
        image.save(output_path)
        print(f"Successfully exported font grid to: '{output_path}'")
    except Exception as e:
        print(f"Error saving image: {e}")
        sys.exit(1)

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python c64font_to_png.py <input_font_file.fnt> [<output_image.png>]")
        sys.exit(1)

    input_file = sys.argv[1]
    output_file = sys.argv[2] if len(sys.argv) > 2 else None

    convert_font_to_png(input_file, output_file)
