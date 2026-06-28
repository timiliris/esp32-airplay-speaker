#!/usr/bin/env python3
"""
make_background.py — Convert a PNG to a raw RGB565 binary for the ST7789 display.

The output is flashed to SPIFFS at /spiffs/bg/background.bin and loaded at
runtime by the display driver. No firmware rebuild is required to change the
background — after running this script, either reflash the SPIFFS image or
upload the file via the HTTP API:

    curl -X POST "http://<device-ip>/api/fs/upload?path=/spiffs/bg/background.bin" \\
         --data-binary @data/bg/background.bin

Usage:
    python3 components/display/make_background.py <source.png> [brightness]

Arguments:
    source.png    Path to the source PNG image (any size — will be resized to 320x170)
    brightness    Optional brightness multiplier, 0.0–1.0 (default: 0.5)
                  The ST7789 backlight renders images significantly brighter than
                  a monitor — 0.5 is a good starting point, adjust to taste.

Output:
    data/bg/background.bin  (relative to project root)

Example:
    python3 components/display/make_background.py ~/Desktop/my_background.png 0.5
    python3 components/display/make_background.py ~/Desktop/my_background.png 0.6

Notes:
    - Output is always 320x170px, little-endian RGB565, 108,800 bytes
    - Byte order confirmed via colour bar test on ST7789 + esp_lvgl_port with swap_bytes=true
    - Floyd-Steinberg dithering is applied to reduce banding on dark gradients
    - The display is 16-bit colour (5R/6G/5B) — subtle gradients will show banding;
      design backgrounds with bold contrast for best results
    - After uploading, the display loads the new background on next boot
"""

import sys
import os

try:
    from PIL import Image, ImageEnhance
except ImportError:
    print("Error: Pillow is not installed.")
    print("Install it with: pip3 install Pillow")
    sys.exit(1)

# ---- Constants ---------------------------------------------------------------

DISPLAY_W          = 320
DISPLAY_H          = 170
DEFAULT_BRIGHTNESS = 0.5

# Script lives in components/display/ — project root is two levels up
SCRIPT_DIR  = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.dirname(os.path.dirname(SCRIPT_DIR))
OUTPUT_DIR  = os.path.join(PROJECT_DIR, 'data', 'bg')
OUTPUT_PATH = os.path.join(OUTPUT_DIR, 'background.bin')

# ---- Main --------------------------------------------------------------------

def convert(source_path, brightness):
    print(f"Source:     {source_path}")
    print(f"Brightness: {brightness}")
    print(f"Output:     {OUTPUT_PATH}")
    print()

    # Load and resize
    img = Image.open(source_path).convert('RGB').resize(
        (DISPLAY_W, DISPLAY_H), Image.LANCZOS
    )

    # Brightness adjustment
    img = ImageEnhance.Brightness(img).enhance(brightness)

    # Floyd-Steinberg dithering — reduces banding on dark gradients at RGB565 depth
    img = img.convert(
        'P', palette=Image.ADAPTIVE, dither=Image.FLOYDSTEINBERG, colors=256
    ).convert('RGB')

    # Convert to little-endian RGB565 raw binary
    pixels = img.load()
    bytes_out = bytearray()
    for y in range(DISPLAY_H):
        for x in range(DISPLAY_W):
            r, g, b = pixels[x, y]
            rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            bytes_out.append(rgb565 & 0xFF)          # low byte first (little-endian)
            bytes_out.append((rgb565 >> 8) & 0xFF)

    # Write raw binary
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    with open(OUTPUT_PATH, 'wb') as f:
        f.write(bytes_out)

    expected = DISPLAY_W * DISPLAY_H * 2
    print(f"Written {len(bytes_out)} bytes ({DISPLAY_W}x{DISPLAY_H} px RGB565)")
    assert len(bytes_out) == expected, f"Size mismatch: {len(bytes_out)} != {expected}"
    print()
    print("Next steps:")
    print("  Flash SPIFFS:  idf.py build && idf.py flash")
    print("  Or upload OTA: curl -X POST \"http://<device-ip>/api/fs/upload?path=/spiffs/bg/background.bin\" \\")
    print(f"                      --data-binary @{OUTPUT_PATH}")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    source = os.path.expanduser(sys.argv[1])
    if not os.path.isfile(source):
        print(f"Error: file not found: {source}")
        sys.exit(1)

    brightness = DEFAULT_BRIGHTNESS
    if len(sys.argv) >= 3:
        try:
            brightness = float(sys.argv[2])
            if not 0.0 < brightness <= 1.0:
                raise ValueError
        except ValueError:
            print(f"Error: brightness must be a number between 0.0 and 1.0 (got '{sys.argv[2]}'")
            sys.exit(1)

    convert(source, brightness)


if __name__ == '__main__':
    main()
