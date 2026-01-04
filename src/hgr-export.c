//  hgr-export.c
//
//  Graphics export for Bobbin Apple II emulator.
//  Supports HGR (Hi-Res) and GR (Lo-Res) modes.
//  Output formats: ASCII art and PPM image.
//
//  Copyright (c) 2025.
//  This code is licensed under the MIT license.

#include "bobbin-internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// =============================================================================
// HGR (Hi-Res Graphics) - 280x192
// =============================================================================

#define HGR_WIDTH   280
#define HGR_HEIGHT  192
#define HGR_BYTES_PER_LINE  40

#define HGR1_BASE   0x2000
#define HGR2_BASE   0x4000

// Apple II HGR artifact colors
static const byte HGR_COLORS[8][3] = {
    {0, 0, 0},       // 0: Black
    {0, 255, 0},     // 1: Green (color set 0, odd column)
    {255, 0, 255},   // 2: Purple (color set 0, even column)
    {255, 255, 255}, // 3: White
    {0, 0, 0},       // 4: Black
    {255, 128, 0},   // 5: Orange (color set 1, odd column)
    {0, 128, 255},   // 6: Blue (color set 1, even column)
    {255, 255, 255}, // 7: White
};

// =============================================================================
// GR (Lo-Res Graphics) - 40x48 with 16 colors
// =============================================================================

#define GR_WIDTH    40
#define GR_HEIGHT   48

#define GR1_BASE    0x0400
#define GR2_BASE    0x0800

// Apple II 16-color Lo-Res palette (RGB values)
// Used for both GR and DGR modes
static const byte GR_COLORS[16][3] = {
    {0, 0, 0},       // 0: Black
    {227, 30, 96},   // 1: Magenta/Red
    {96, 78, 189},   // 2: Dark Blue
    {255, 68, 253},  // 3: Purple/Violet
    {0, 163, 96},    // 4: Dark Green
    {156, 156, 156}, // 5: Grey 1 (Dark)
    {20, 207, 253},  // 6: Medium Blue
    {208, 195, 255}, // 7: Light Blue
    {96, 114, 3},    // 8: Brown
    {255, 106, 60},  // 9: Orange
    {156, 156, 156}, // 10: Grey 2 (Light)
    {255, 160, 208}, // 11: Pink
    {20, 245, 60},   // 12: Green
    {208, 221, 141}, // 13: Yellow
    {114, 255, 208}, // 14: Aqua/Cyan
    {255, 255, 255}, // 15: White
};

// =============================================================================
// DHGR (Double Hi-Res Graphics) - 560x192, //e only
// =============================================================================

#define DHGR_WIDTH   560
#define DHGR_HEIGHT  192

// DHGR uses same base addresses as HGR, but reads from both main and aux memory
// Aux memory offset (from apple2.h)
#define AUX_OFFSET   0x10000

// =============================================================================
// DGR (Double Lo-Res Graphics) - 80x48, //e only
// =============================================================================

#define DGR_WIDTH    80
#define DGR_HEIGHT   48

// DGR uses same base addresses as GR, but reads from both main and aux memory

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

                fputc(HGR_COLORS[color_idx][0], f);
                fputc(HGR_COLORS[color_idx][1], f);
                fputc(HGR_COLORS[color_idx][2], f);
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

// =============================================================================
// GR (Lo-Res Graphics) Functions
// =============================================================================

// GR memory layout is same as text screen (interleaved)
// Each byte contains 2 vertically-stacked pixels:
//   - Low nibble (bits 0-3) = top pixel
//   - High nibble (bits 4-7) = bottom pixel

// Text/GR line addresses (same interleaving pattern)
static const word GR_LINE_OFFSETS[24] = {
    0x000, 0x080, 0x100, 0x180, 0x200, 0x280, 0x300, 0x380,  // Lines 0-7
    0x028, 0x0A8, 0x128, 0x1A8, 0x228, 0x2A8, 0x328, 0x3A8,  // Lines 8-15
    0x050, 0x0D0, 0x150, 0x1D0, 0x250, 0x2D0, 0x350, 0x3D0,  // Lines 16-23
};

// Get address for a GR text row (each text row = 2 GR pixel rows)
static word gr_row_addr(int text_row, word base)
{
    if (text_row < 0 || text_row >= 24) return base;
    return base + GR_LINE_OFFSETS[text_row];
}

// Get color of a single GR pixel
static int gr_get_pixel(word base, int x, int y)
{
    if (x < 0 || x >= GR_WIDTH || y < 0 || y >= GR_HEIGHT) {
        return 0;
    }

    int text_row = y / 2;  // Which text row (0-23)
    int is_bottom = y % 2; // Top or bottom pixel in the byte

    word addr = gr_row_addr(text_row, base) + x;
    byte data = peek_sneaky(addr);

    if (is_bottom) {
        return (data >> 4) & 0x0F;  // High nibble
    } else {
        return data & 0x0F;          // Low nibble
    }
}

// Export GR to ASCII art
// Uses hex digits 0-9, A-F to represent colors
int gr_export_ascii(word base, const char *filename)
{
    FILE *f = fopen(filename, "w");
    if (!f) {
        return -1;
    }

    // Use hex digits for color values
    const char *hex = "0123456789ABCDEF";

    for (int y = 0; y < GR_HEIGHT; y++) {
        for (int x = 0; x < GR_WIDTH; x++) {
            int color = gr_get_pixel(base, x, y);
            // Double each character for better aspect ratio
            fputc(hex[color], f);
            fputc(hex[color], f);
        }
        fputc('\n', f);
    }

    fclose(f);
    return 0;
}

// Export GR to PPM format with true colors
int gr_export_ppm(word base, const char *filename)
{
    FILE *f = fopen(filename, "wb");
    if (!f) {
        return -1;
    }

    // Scale up for visibility: each GR pixel becomes 7x4 output pixels
    // This gives 280x192, same as HGR
    int scale_x = 7;
    int scale_y = 4;
    int out_width = GR_WIDTH * scale_x;   // 40 * 7 = 280
    int out_height = GR_HEIGHT * scale_y; // 48 * 4 = 192

    fprintf(f, "P6\n%d %d\n255\n", out_width, out_height);

    for (int y = 0; y < GR_HEIGHT; y++) {
        // Repeat each row scale_y times
        for (int sy = 0; sy < scale_y; sy++) {
            for (int x = 0; x < GR_WIDTH; x++) {
                int color = gr_get_pixel(base, x, y);

                // Repeat each pixel scale_x times
                for (int sx = 0; sx < scale_x; sx++) {
                    fputc(GR_COLORS[color][0], f);
                    fputc(GR_COLORS[color][1], f);
                    fputc(GR_COLORS[color][2], f);
                }
            }
        }
    }

    fclose(f);
    return 0;
}

// Export GR to PPM at native resolution (40x48)
int gr_export_ppm_native(word base, const char *filename)
{
    FILE *f = fopen(filename, "wb");
    if (!f) {
        return -1;
    }

    fprintf(f, "P6\n%d %d\n255\n", GR_WIDTH, GR_HEIGHT);

    for (int y = 0; y < GR_HEIGHT; y++) {
        for (int x = 0; x < GR_WIDTH; x++) {
            int color = gr_get_pixel(base, x, y);
            fputc(GR_COLORS[color][0], f);
            fputc(GR_COLORS[color][1], f);
            fputc(GR_COLORS[color][2], f);
        }
    }

    fclose(f);
    return 0;
}

// =============================================================================
// DHGR (Double Hi-Res Graphics) Functions
// =============================================================================

// DHGR uses both main and aux memory, interleaved by byte:
// - Even byte columns (0,2,4...) come from AUX memory
// - Odd byte columns (1,3,5...) come from MAIN memory
// Total 80 bytes per line (40 aux + 40 main) = 560 pixels

// Check if aux memory is available (//e with 128KB)
static bool have_aux_memory(void)
{
    return cfg.amt_ram > AUX_OFFSET;
}

// Get a single pixel from DHGR memory (0 or 1 for mono)
static int dhgr_get_pixel(word base, int x, int y)
{
    if (x < 0 || x >= DHGR_WIDTH || y < 0 || y >= DHGR_HEIGHT) {
        return 0;
    }

    const byte *mem = getram();
    word line_addr = hgr_line_addr(y, base);

    // In DHGR, bytes are interleaved: aux0, main0, aux1, main1, ...
    // Each byte has 7 pixels, so 80 bytes = 560 pixels
    int byte_col = x / 7;      // Which byte (0-79)
    int bit = x % 7;           // Which bit within byte (0-6)

    // Even byte columns from aux, odd from main
    word addr;
    if (byte_col % 2 == 0) {
        // Even byte column -> aux memory
        addr = AUX_OFFSET + line_addr + (byte_col / 2);
    } else {
        // Odd byte column -> main memory
        addr = line_addr + (byte_col / 2);
    }

    byte data = mem[addr];
    return (data >> bit) & 1;
}

// Export DHGR to ASCII art
int dhgr_export_ascii(word base, const char *filename, int scale)
{
    if (!have_aux_memory()) {
        return -3;  // No aux memory
    }

    FILE *f = fopen(filename, "w");
    if (!f) {
        return -1;
    }

    const char *density = " .:-=+*#@";
    int cell_w = (scale > 1) ? scale : 2;
    int cell_h = (scale > 1) ? scale : 2;

    for (int cy = 0; cy < DHGR_HEIGHT; cy += cell_h) {
        for (int cx = 0; cx < DHGR_WIDTH; cx += cell_w) {
            int count = 0;
            int max_count = 0;

            for (int dy = 0; dy < cell_h && (cy + dy) < DHGR_HEIGHT; dy++) {
                for (int dx = 0; dx < cell_w && (cx + dx) < DHGR_WIDTH; dx++) {
                    count += dhgr_get_pixel(base, cx + dx, cy + dy);
                    max_count++;
                }
            }

            int idx = (count * 8) / max_count;
            if (idx > 8) idx = 8;
            fputc(density[idx], f);
        }
        fputc('\n', f);
    }

    fclose(f);
    return 0;
}

// Export DHGR to PPM (mono)
int dhgr_export_ppm(word base, const char *filename)
{
    if (!have_aux_memory()) {
        return -3;  // No aux memory
    }

    FILE *f = fopen(filename, "wb");
    if (!f) {
        return -1;
    }

    fprintf(f, "P6\n%d %d\n255\n", DHGR_WIDTH, DHGR_HEIGHT);

    for (int y = 0; y < DHGR_HEIGHT; y++) {
        for (int x = 0; x < DHGR_WIDTH; x++) {
            int pixel = dhgr_get_pixel(base, x, y);
            byte val = pixel ? 255 : 0;
            fputc(val, f);
            fputc(val, f);
            fputc(val, f);
        }
    }

    fclose(f);
    return 0;
}

// =============================================================================
// DGR (Double Lo-Res Graphics) Functions
// =============================================================================

// DGR uses both main and aux memory, interleaved by column:
// - Even columns (0,2,4...) come from AUX memory
// - Odd columns (1,3,5...) come from MAIN memory
// Total 80 columns, each byte still has 2 pixels (top/bottom nibbles)

// Get color of a single DGR pixel
static int dgr_get_pixel(word base, int x, int y)
{
    if (x < 0 || x >= DGR_WIDTH || y < 0 || y >= DGR_HEIGHT) {
        return 0;
    }

    const byte *mem = getram();

    int text_row = y / 2;  // Which text row (0-23)
    int is_bottom = y % 2; // Top or bottom pixel in the byte
    int main_col = x / 2;  // Column in main/aux memory (0-39)
    bool is_aux = (x % 2 == 0);  // Even columns from aux

    word addr = gr_row_addr(text_row, base) + main_col;
    if (is_aux) {
        addr += AUX_OFFSET;
    }

    byte data = mem[addr];

    if (is_bottom) {
        return (data >> 4) & 0x0F;  // High nibble
    } else {
        return data & 0x0F;          // Low nibble
    }
}

// Export DGR to ASCII art (hex digits)
int dgr_export_ascii(word base, const char *filename)
{
    if (!have_aux_memory()) {
        return -3;  // No aux memory
    }

    FILE *f = fopen(filename, "w");
    if (!f) {
        return -1;
    }

    const char *hex = "0123456789ABCDEF";

    for (int y = 0; y < DGR_HEIGHT; y++) {
        for (int x = 0; x < DGR_WIDTH; x++) {
            int color = dgr_get_pixel(base, x, y);
            fputc(hex[color], f);
        }
        fputc('\n', f);
    }

    fclose(f);
    return 0;
}

// Export DGR to PPM (scaled to 560x192 for consistent size)
int dgr_export_ppm(word base, const char *filename)
{
    if (!have_aux_memory()) {
        return -3;  // No aux memory
    }

    FILE *f = fopen(filename, "wb");
    if (!f) {
        return -1;
    }

    // Scale up: each DGR pixel becomes 7x4 output pixels
    // This gives 560x192, same as DHGR
    int scale_x = 7;
    int scale_y = 4;
    int out_width = DGR_WIDTH * scale_x;   // 80 * 7 = 560
    int out_height = DGR_HEIGHT * scale_y; // 48 * 4 = 192

    fprintf(f, "P6\n%d %d\n255\n", out_width, out_height);

    for (int y = 0; y < DGR_HEIGHT; y++) {
        for (int sy = 0; sy < scale_y; sy++) {
            for (int x = 0; x < DGR_WIDTH; x++) {
                int color = dgr_get_pixel(base, x, y);

                for (int sx = 0; sx < scale_x; sx++) {
                    fputc(GR_COLORS[color][0], f);
                    fputc(GR_COLORS[color][1], f);
                    fputc(GR_COLORS[color][2], f);
                }
            }
        }
    }

    fclose(f);
    return 0;
}

// Export DGR to PPM at native resolution (80x48)
int dgr_export_ppm_native(word base, const char *filename)
{
    if (!have_aux_memory()) {
        return -3;  // No aux memory
    }

    FILE *f = fopen(filename, "wb");
    if (!f) {
        return -1;
    }

    fprintf(f, "P6\n%d %d\n255\n", DGR_WIDTH, DGR_HEIGHT);

    for (int y = 0; y < DGR_HEIGHT; y++) {
        for (int x = 0; x < DGR_WIDTH; x++) {
            int color = dgr_get_pixel(base, x, y);
            fputc(GR_COLORS[color][0], f);
            fputc(GR_COLORS[color][1], f);
            fputc(GR_COLORS[color][2], f);
        }
    }

    fclose(f);
    return 0;
}

// =============================================================================
// Command Handlers
// =============================================================================

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

// GR (Lo-Res) command handler
bool gr_command_do(const char *line, printer pr)
{
    word base = 0;
    const char *filename = NULL;
    int cmd_type = 0;  // 1=ascii, 2=ppm (scaled), 3=ppm (native)

    // GR1 ASCII
    if (!cmd_type && (filename = check_prefix(line, "save-gr-ascii ")) != NULL) {
        base = GR1_BASE; cmd_type = 1;
    }
    if (!cmd_type && (filename = check_prefix(line, "sga ")) != NULL) {
        base = GR1_BASE; cmd_type = 1;
    }

    // GR1 PPM (scaled to 280x192)
    if (!cmd_type && (filename = check_prefix(line, "save-gr-ppm ")) != NULL) {
        base = GR1_BASE; cmd_type = 2;
    }
    if (!cmd_type && (filename = check_prefix(line, "sgp ")) != NULL) {
        base = GR1_BASE; cmd_type = 2;
    }

    // GR1 PPM native (40x48)
    if (!cmd_type && (filename = check_prefix(line, "save-gr-ppm-native ")) != NULL) {
        base = GR1_BASE; cmd_type = 3;
    }

    // GR2 ASCII
    if (!cmd_type && (filename = check_prefix(line, "save-gr2-ascii ")) != NULL) {
        base = GR2_BASE; cmd_type = 1;
    }
    if (!cmd_type && (filename = check_prefix(line, "sga2 ")) != NULL) {
        base = GR2_BASE; cmd_type = 1;
    }

    // GR2 PPM (scaled)
    if (!cmd_type && (filename = check_prefix(line, "save-gr2-ppm ")) != NULL) {
        base = GR2_BASE; cmd_type = 2;
    }
    if (!cmd_type && (filename = check_prefix(line, "sgp2 ")) != NULL) {
        base = GR2_BASE; cmd_type = 2;
    }

    // GR2 PPM native
    if (!cmd_type && (filename = check_prefix(line, "save-gr2-ppm-native ")) != NULL) {
        base = GR2_BASE; cmd_type = 3;
    }

    if (!cmd_type) {
        return false;  // Not a GR command
    }

    // Skip whitespace in filename
    while (*filename == ' ') filename++;

    if (*filename == '\0') {
        pr("ERR: Missing filename\n");
        return true;
    }

    int result;
    const char *page = (base == GR1_BASE) ? "GR1" : "GR2";

    switch (cmd_type) {
        case 1:  // ASCII
            result = gr_export_ascii(base, filename);
            if (result == 0) {
                pr("Saved %s to ASCII file \"%s\" (40x48).\n", page, filename);
            } else {
                pr("ERR: Could not save to \"%s\": %s\n", filename, strerror(errno));
            }
            break;

        case 2:  // PPM scaled
            result = gr_export_ppm(base, filename);
            if (result == 0) {
                pr("Saved %s to PPM file \"%s\" (280x192, 16 colors).\n", page, filename);
            } else {
                pr("ERR: Could not save to \"%s\": %s\n", filename, strerror(errno));
            }
            break;

        case 3:  // PPM native
            result = gr_export_ppm_native(base, filename);
            if (result == 0) {
                pr("Saved %s to PPM file \"%s\" (40x48, native).\n", page, filename);
            } else {
                pr("ERR: Could not save to \"%s\": %s\n", filename, strerror(errno));
            }
            break;
    }

    return true;
}

// DHGR (Double Hi-Res) command handler
bool dhgr_command_do(const char *line, printer pr)
{
    word base = 0;
    const char *filename = NULL;
    int cmd_type = 0;  // 1=ascii, 2=ppm

    // DHGR1 ASCII
    if (!cmd_type && (filename = check_prefix(line, "save-dhgr-ascii ")) != NULL) {
        base = HGR1_BASE; cmd_type = 1;
    }
    if (!cmd_type && (filename = check_prefix(line, "sdha ")) != NULL) {
        base = HGR1_BASE; cmd_type = 1;
    }

    // DHGR1 PPM
    if (!cmd_type && (filename = check_prefix(line, "save-dhgr-ppm ")) != NULL) {
        base = HGR1_BASE; cmd_type = 2;
    }
    if (!cmd_type && (filename = check_prefix(line, "sdhp ")) != NULL) {
        base = HGR1_BASE; cmd_type = 2;
    }

    // DHGR2 ASCII
    if (!cmd_type && (filename = check_prefix(line, "save-dhgr2-ascii ")) != NULL) {
        base = HGR2_BASE; cmd_type = 1;
    }
    if (!cmd_type && (filename = check_prefix(line, "sdha2 ")) != NULL) {
        base = HGR2_BASE; cmd_type = 1;
    }

    // DHGR2 PPM
    if (!cmd_type && (filename = check_prefix(line, "save-dhgr2-ppm ")) != NULL) {
        base = HGR2_BASE; cmd_type = 2;
    }
    if (!cmd_type && (filename = check_prefix(line, "sdhp2 ")) != NULL) {
        base = HGR2_BASE; cmd_type = 2;
    }

    if (!cmd_type) {
        return false;  // Not a DHGR command
    }

    while (*filename == ' ') filename++;

    if (*filename == '\0') {
        pr("ERR: Missing filename\n");
        return true;
    }

    if (!have_aux_memory()) {
        pr("ERR: DHGR requires //e with 128KB RAM (aux memory not available)\n");
        return true;
    }

    int result;
    const char *page = (base == HGR1_BASE) ? "DHGR1" : "DHGR2";

    switch (cmd_type) {
        case 1:  // ASCII
            result = dhgr_export_ascii(base, filename, 2);
            if (result == 0) {
                pr("Saved %s to ASCII file \"%s\".\n", page, filename);
            } else {
                pr("ERR: Could not save to \"%s\": %s\n", filename, strerror(errno));
            }
            break;

        case 2:  // PPM
            result = dhgr_export_ppm(base, filename);
            if (result == 0) {
                pr("Saved %s to PPM file \"%s\" (560x192, mono).\n", page, filename);
            } else {
                pr("ERR: Could not save to \"%s\": %s\n", filename, strerror(errno));
            }
            break;
    }

    return true;
}

// DGR (Double Lo-Res) command handler
bool dgr_command_do(const char *line, printer pr)
{
    word base = 0;
    const char *filename = NULL;
    int cmd_type = 0;  // 1=ascii, 2=ppm (scaled), 3=ppm (native)

    // DGR1 ASCII
    if (!cmd_type && (filename = check_prefix(line, "save-dgr-ascii ")) != NULL) {
        base = GR1_BASE; cmd_type = 1;
    }
    if (!cmd_type && (filename = check_prefix(line, "sdga ")) != NULL) {
        base = GR1_BASE; cmd_type = 1;
    }

    // DGR1 PPM (scaled to 560x192)
    if (!cmd_type && (filename = check_prefix(line, "save-dgr-ppm ")) != NULL) {
        base = GR1_BASE; cmd_type = 2;
    }
    if (!cmd_type && (filename = check_prefix(line, "sdgp ")) != NULL) {
        base = GR1_BASE; cmd_type = 2;
    }

    // DGR1 PPM native (80x48)
    if (!cmd_type && (filename = check_prefix(line, "save-dgr-ppm-native ")) != NULL) {
        base = GR1_BASE; cmd_type = 3;
    }

    // DGR2 ASCII
    if (!cmd_type && (filename = check_prefix(line, "save-dgr2-ascii ")) != NULL) {
        base = GR2_BASE; cmd_type = 1;
    }
    if (!cmd_type && (filename = check_prefix(line, "sdga2 ")) != NULL) {
        base = GR2_BASE; cmd_type = 1;
    }

    // DGR2 PPM (scaled)
    if (!cmd_type && (filename = check_prefix(line, "save-dgr2-ppm ")) != NULL) {
        base = GR2_BASE; cmd_type = 2;
    }
    if (!cmd_type && (filename = check_prefix(line, "sdgp2 ")) != NULL) {
        base = GR2_BASE; cmd_type = 2;
    }

    // DGR2 PPM native
    if (!cmd_type && (filename = check_prefix(line, "save-dgr2-ppm-native ")) != NULL) {
        base = GR2_BASE; cmd_type = 3;
    }

    if (!cmd_type) {
        return false;  // Not a DGR command
    }

    while (*filename == ' ') filename++;

    if (*filename == '\0') {
        pr("ERR: Missing filename\n");
        return true;
    }

    if (!have_aux_memory()) {
        pr("ERR: DGR requires //e with 128KB RAM (aux memory not available)\n");
        return true;
    }

    int result;
    const char *page = (base == GR1_BASE) ? "DGR1" : "DGR2";

    switch (cmd_type) {
        case 1:  // ASCII
            result = dgr_export_ascii(base, filename);
            if (result == 0) {
                pr("Saved %s to ASCII file \"%s\" (80x48).\n", page, filename);
            } else {
                pr("ERR: Could not save to \"%s\": %s\n", filename, strerror(errno));
            }
            break;

        case 2:  // PPM scaled
            result = dgr_export_ppm(base, filename);
            if (result == 0) {
                pr("Saved %s to PPM file \"%s\" (560x192, 16 colors).\n", page, filename);
            } else {
                pr("ERR: Could not save to \"%s\": %s\n", filename, strerror(errno));
            }
            break;

        case 3:  // PPM native
            result = dgr_export_ppm_native(base, filename);
            if (result == 0) {
                pr("Saved %s to PPM file \"%s\" (80x48, native).\n", page, filename);
            } else {
                pr("ERR: Could not save to \"%s\": %s\n", filename, strerror(errno));
            }
            break;
    }

    return true;
}
