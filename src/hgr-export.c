//  hgr-export.c
//
//  HGR (Hi-Res Graphics) export for Bobbin Apple II emulator.
//  Supports ASCII art, PPM, and PNG output formats.
//
//  Copyright (c) 2025.
//  This code is licensed under the MIT license.

#include "bobbin-internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// HGR dimensions
#define HGR_WIDTH   280
#define HGR_HEIGHT  192
#define HGR_BYTES_PER_LINE  40

// HGR base addresses
#define HGR1_BASE   0x2000
#define HGR2_BASE   0x4000

// Apple II HGR colors (for color mode)
// In mono mode, we just care about on/off
static const byte APPLE_COLORS[8][3] = {
    {0, 0, 0},       // 0: Black
    {0, 255, 0},     // 1: Green (color set 0, odd column)
    {255, 0, 255},   // 2: Purple (color set 0, even column)
    {255, 255, 255}, // 3: White
    {0, 0, 0},       // 4: Black
    {255, 128, 0},   // 5: Orange (color set 1, odd column)
    {0, 128, 255},   // 6: Blue (color set 1, even column)
    {255, 255, 255}, // 7: White
};

// Calculate the memory address for a given HGR line
static word hgr_line_addr(int line, word base)
{
    // Apple II HGR memory layout is interleaved:
    // Lines 0,8,16,24... are in one 1KB block
    // Lines 1,9,17,25... are in the next 1KB block
    // etc.
    //
    // Within each 1KB block, there are 8 groups of 128 bytes
    // Formula: base + (line % 8) * 0x400 + (line / 64) * 0x28 + ((line / 8) % 8) * 0x80

    int group = line % 8;           // Which 1KB block (0-7)
    int third = line / 64;          // Which third of the screen (0-2)
    int row_in_group = (line / 8) % 8;  // Which row within the group (0-7)

    return base + (group * 0x400) + (third * 0x28) + (row_in_group * 0x80);
}

// Get a single pixel from HGR memory (0 or 1 for mono)
static int hgr_get_pixel(word base, int x, int y)
{
    if (x < 0 || x >= HGR_WIDTH || y < 0 || y >= HGR_HEIGHT) {
        return 0;
    }

    word line_addr = hgr_line_addr(y, base);
    int byte_offset = x / 7;
    int bit_offset = x % 7;  // Bits 0-6 are pixels, bit 7 is color

    byte data = peek_sneaky(line_addr + byte_offset);
    return (data >> bit_offset) & 1;
}

// Export HGR to ASCII art
// Uses different density characters based on pixel count in each cell
int hgr_export_ascii(word base, const char *filename, int scale)
{
    FILE *f = fopen(filename, "w");
    if (!f) {
        return -1;
    }

    // Characters for different densities (0-7 pixels lit in a cell)
    const char *density = " .:-=+*#@";

    // Use 2x2 pixel cells for ASCII output
    int cell_w = 2;
    int cell_h = 2;

    if (scale > 1) {
        cell_w = scale;
        cell_h = scale;
    }

    for (int cy = 0; cy < HGR_HEIGHT; cy += cell_h) {
        for (int cx = 0; cx < HGR_WIDTH; cx += cell_w) {
            int count = 0;
            int max_count = 0;

            // Count lit pixels in this cell
            for (int dy = 0; dy < cell_h && (cy + dy) < HGR_HEIGHT; dy++) {
                for (int dx = 0; dx < cell_w && (cx + dx) < HGR_WIDTH; dx++) {
                    count += hgr_get_pixel(base, cx + dx, cy + dy);
                    max_count++;
                }
            }

            // Map to density character (0-8 scale)
            int idx = (count * 8) / max_count;
            if (idx > 8) idx = 8;
            fputc(density[idx], f);
        }
        fputc('\n', f);
    }

    fclose(f);
    return 0;
}

// Export HGR to PPM format (Portable Pixel Map)
// Simple uncompressed RGB format that's easy to generate
int hgr_export_ppm(word base, const char *filename, bool color_mode)
{
    FILE *f = fopen(filename, "wb");
    if (!f) {
        return -1;
    }

    // PPM header: P6 = binary RGB
    fprintf(f, "P6\n%d %d\n255\n", HGR_WIDTH, HGR_HEIGHT);

    for (int y = 0; y < HGR_HEIGHT; y++) {
        word line_addr = hgr_line_addr(y, base);

        for (int x = 0; x < HGR_WIDTH; x++) {
            int byte_offset = x / 7;
            int bit_offset = x % 7;

            byte data = peek_sneaky(line_addr + byte_offset);
            int pixel = (data >> bit_offset) & 1;

            if (color_mode) {
                // Color mode: use Apple II color artifacts
                int color_set = (data >> 7) & 1;  // Bit 7 selects color set
                int col_type = (x % 2);           // Even/odd column

                int color_idx;
                if (!pixel) {
                    color_idx = 0;  // Black
                } else {
                    // Color depends on color set and column parity
                    color_idx = 1 + color_set * 4 + col_type;
                    if (color_idx > 7) color_idx = 7;
                }

                fputc(APPLE_COLORS[color_idx][0], f);
                fputc(APPLE_COLORS[color_idx][1], f);
                fputc(APPLE_COLORS[color_idx][2], f);
            } else {
                // Mono mode: just white or black
                byte val = pixel ? 255 : 0;
                fputc(val, f);
                fputc(val, f);
                fputc(val, f);
            }
        }
    }

    fclose(f);
    return 0;
}

// Export HGR to PNG format
// This is more complex - we'll use a minimal PNG encoder
// For now, return -2 to indicate "not implemented"
int hgr_export_png(word base, const char *filename, bool color_mode)
{
    (void)base;
    (void)filename;
    (void)color_mode;

    // PNG requires compression (zlib) which adds complexity
    // For now, recommend using PPM and external conversion
    return -2;  // Not implemented
}

// Helper to check if line starts with prefix and get filename
static const char *check_prefix(const char *line, const char *prefix)
{
    size_t len = strlen(prefix);
    if (memcmp(line, prefix, len) == 0) {
        return line + len;
    }
    return NULL;
}

// Command handler for save-hgr commands
// Returns true if command was handled
bool hgr_command_do(const char *line, printer pr)
{
    word base = 0;
    const char *filename = NULL;
    int cmd_type = 0;  // 1=ascii, 2=ppm, 3=png

    // HGR1 ASCII
    if (!cmd_type && (filename = check_prefix(line, "save-hgr-ascii ")) != NULL) {
        base = HGR1_BASE; cmd_type = 1;
    }
    if (!cmd_type && (filename = check_prefix(line, "sha ")) != NULL) {
        base = HGR1_BASE; cmd_type = 1;
    }

    // HGR1 PPM
    if (!cmd_type && (filename = check_prefix(line, "save-hgr-ppm ")) != NULL) {
        base = HGR1_BASE; cmd_type = 2;
    }
    if (!cmd_type && (filename = check_prefix(line, "shp ")) != NULL) {
        base = HGR1_BASE; cmd_type = 2;
    }

    // HGR1 PNG
    if (!cmd_type && (filename = check_prefix(line, "save-hgr-png ")) != NULL) {
        base = HGR1_BASE; cmd_type = 3;
    }

    // HGR2 ASCII
    if (!cmd_type && (filename = check_prefix(line, "save-hgr2-ascii ")) != NULL) {
        base = HGR2_BASE; cmd_type = 1;
    }
    if (!cmd_type && (filename = check_prefix(line, "sha2 ")) != NULL) {
        base = HGR2_BASE; cmd_type = 1;
    }

    // HGR2 PPM
    if (!cmd_type && (filename = check_prefix(line, "save-hgr2-ppm ")) != NULL) {
        base = HGR2_BASE; cmd_type = 2;
    }
    if (!cmd_type && (filename = check_prefix(line, "shp2 ")) != NULL) {
        base = HGR2_BASE; cmd_type = 2;
    }

    // HGR2 PNG
    if (!cmd_type && (filename = check_prefix(line, "save-hgr2-png ")) != NULL) {
        base = HGR2_BASE; cmd_type = 3;
    }

    if (!cmd_type) {
        return false;  // Not an HGR command
    }

    // Skip whitespace in filename
    while (*filename == ' ') filename++;

    if (*filename == '\0') {
        pr("ERR: Missing filename\n");
        return true;
    }

    int result;
    const char *page = (base == HGR1_BASE) ? "HGR1" : "HGR2";

    switch (cmd_type) {
        case 1:  // ASCII
            result = hgr_export_ascii(base, filename, 2);
            if (result == 0) {
                pr("Saved %s to ASCII file \"%s\".\n", page, filename);
            } else {
                pr("ERR: Could not save to \"%s\": %s\n", filename, strerror(errno));
            }
            break;

        case 2:  // PPM
            result = hgr_export_ppm(base, filename, false);  // mono mode
            if (result == 0) {
                pr("Saved %s to PPM file \"%s\" (280x192, mono).\n", page, filename);
            } else {
                pr("ERR: Could not save to \"%s\": %s\n", filename, strerror(errno));
            }
            break;

        case 3:  // PNG
            pr("PNG export not yet implemented. Use PPM and convert:\n");
            pr("  convert %s output.png\n", filename);
            break;
    }

    return true;
}

// Color mode variants
bool hgr_command_do_color(const char *line, printer pr)
{
    const char *SAVE_HGR_PPM_COLOR = "save-hgr-ppm-color ";
    const char *SAVE_HGR2_PPM_COLOR = "save-hgr2-ppm-color ";

    word base = 0;
    const char *filename = NULL;

    size_t len1 = strlen(SAVE_HGR_PPM_COLOR);
    size_t len2 = strlen(SAVE_HGR2_PPM_COLOR);

    if (!memcmp(line, SAVE_HGR_PPM_COLOR, len1)) {
        base = HGR1_BASE;
        filename = line + len1;
    } else if (!memcmp(line, SAVE_HGR2_PPM_COLOR, len2)) {
        base = HGR2_BASE;
        filename = line + len2;
    } else {
        return false;
    }

    while (*filename == ' ') filename++;

    if (*filename == '\0') {
        pr("ERR: Missing filename\n");
        return true;
    }

    int result = hgr_export_ppm(base, filename, true);  // color mode
    const char *page = (base == HGR1_BASE) ? "HGR1" : "HGR2";

    if (result == 0) {
        pr("Saved %s to PPM file \"%s\" (280x192, color).\n", page, filename);
    } else {
        pr("ERR: Could not save to \"%s\": %s\n", filename, strerror(errno));
    }

    return true;
}
