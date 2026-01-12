//  periph/mouse.c
//
//  AppleMouse card emulation for Bobbin Apple II Emulator
//  Uses actual Apple Mouse Interface ROM (342-0270-C)
//
//  Copyright (c) 2024 Bobbin Contributors
//  This code is licensed under the MIT license.
//  See the accompanying LICENSE file for details.

#include "bobbin-internal.h"

#include <stdio.h>
#include <string.h>

// ROM size and structure
#define MOUSE_ROM_SIZE      2048    // 2KB ROM
#define MOUSE_ROM_PAGES     8       // 8 pages of 256 bytes
#define MOUSE_PAGE_SIZE     256

// Screen hole locations (indexed by slot number)
#define MOUSE_X_LOW_BASE    0x0478  // +slot = X low byte
#define MOUSE_X_HIGH_BASE   0x0578  // +slot = X high byte
#define MOUSE_Y_LOW_BASE    0x04F8  // +slot = Y low byte
#define MOUSE_Y_HIGH_BASE   0x05F8  // +slot = Y high byte
#define MOUSE_STATUS_BASE   0x0778  // +slot = status/button
#define MOUSE_MODE_BASE     0x07F8  // +slot = mode/slot ID

// 6821 PIA register offsets (from $C0n0)
#define PIA_ORA     0   // Output Register A / Data Direction A
#define PIA_CRA     1   // Control Register A
#define PIA_ORB     2   // Output Register B / Data Direction B
#define PIA_CRB     3   // Control Register B

// Mouse state
typedef struct {
    word x;             // X position (0-1023)
    word y;             // Y position (0-1023)
    bool button;        // Button pressed
    byte mode;          // Current mode

    // 6821 PIA state
    byte pia_ora;       // Output Register A
    byte pia_orb;       // Output Register B (includes ROM page select)
    byte pia_cra;       // Control Register A
    byte pia_crb;       // Control Register B
    byte pia_ddra;      // Data Direction Register A
    byte pia_ddrb;      // Data Direction Register B

    // Movement accumulators (simulates 68705 MCU)
    int delta_x;
    int delta_y;
} MouseState;

static MouseState mouse = {0};
static unsigned int slot_num = 4;  // Default to slot 4

// ROM data - loaded from file
static byte mouse_rom[MOUSE_ROM_SIZE];
static bool rom_loaded = false;

// Forward declarations
static void load_mouse_rom(void);
static byte get_rom_page(void);

//=============================================================================
// Public API for setting mouse state (called by MCP or debugger)
//=============================================================================

void mouse_set_position(word x, word y)
{
    // Calculate delta for quadrature simulation
    mouse.delta_x += (int)x - (int)mouse.x;
    mouse.delta_y += (int)y - (int)mouse.y;

    mouse.x = x;
    mouse.y = y;
    DEBUG("Mouse: Position set to (%d, %d)\n", x, y);
}

void mouse_set_button(bool pressed)
{
    mouse.button = pressed;
    DEBUG("Mouse: Button %s\n", pressed ? "pressed" : "released");
}

void mouse_get_state(word *x, word *y, bool *button)
{
    if (x) *x = mouse.x;
    if (y) *y = mouse.y;
    if (button) *button = mouse.button;
}

//=============================================================================
// ROM loading
//=============================================================================

static void load_mouse_rom(void)
{
    // Try multiple paths for the ROM file
    const char *rom_paths[] = {
        "roms/cards/mouse.rom",
        "../roms/cards/mouse.rom",
        ROMSRCHDIR "/cards/mouse.rom",
        // Absolute path for MCP server running from different directory
        "/Users/redwolf/projects/apple2-unified/bobbin/src/roms/cards/mouse.rom",
        "/Users/redwolf/projects/apple2-unified/Apple Mouse Interface Card ROM - 342-0270-C.bin",
        NULL
    };

    for (const char **path = rom_paths; *path != NULL; path++) {
        FILE *f = fopen(*path, "rb");
        if (f) {
            size_t n = fread(mouse_rom, 1, MOUSE_ROM_SIZE, f);
            fclose(f);
            if (n == MOUSE_ROM_SIZE) {
                rom_loaded = true;
                fprintf(stderr, "Mouse: Loaded ROM from %s\n", *path);
                return;
            }
        }
    }

    fprintf(stderr, "Mouse: Could not load ROM file, using minimal firmware\n");
    rom_loaded = false;

    // Initialize minimal ROM with RTS instructions at entry points
    memset(mouse_rom, 0x00, MOUSE_ROM_SIZE);

    // Signature bytes (page 0)
    mouse_rom[0x05] = 0x38;
    mouse_rom[0x07] = 0x18;
    mouse_rom[0x0B] = 0x01;
    mouse_rom[0x0C] = 0x20;
    mouse_rom[0xFB] = 0xD6;

    // Entry points return RTS
    mouse_rom[0x12] = 0x60;  // SETMOUSE
    mouse_rom[0x13] = 0x60;  // SERVEMOUSE
    mouse_rom[0x14] = 0x60;  // READMOUSE
    mouse_rom[0x16] = 0x60;  // POSMOUSE
    mouse_rom[0x17] = 0x60;  // CLAMPMOUSE
    mouse_rom[0x18] = 0x60;  // CLEARMOUSE
    mouse_rom[0x19] = 0x60;  // INITMOUSE
    mouse_rom[0x1C] = 0x60;  // TIMEDATA
}

// Get current ROM page based on PIA ORB
static byte get_rom_page(void)
{
    // ROM page is selected by bits 0-2 of ORB (directly, no shift needed here
    // because the actual banking maps ORB bit 7 to ROM A10, etc.)
    // Based on MAME: offset = (m_by6821B << 7) & 0x0700
    // So ORB bits 0-2 select which 256-byte page
    return (mouse.pia_orb >> 0) & 0x07;
}

//=============================================================================
// 6821 PIA emulation
//=============================================================================

static byte pia_read(int reg)
{
    switch (reg) {
        case PIA_ORA:
            if (mouse.pia_cra & 0x04) {
                // Read Output Register A - returns mouse data from "MCU"
                // Simulate quadrature signals:
                // Bit 0: X0 (toggles per X movement)
                // Bit 1: X1 (X direction: 0=left, 1=right)
                // Bit 2: Y0 (Y direction: 0=up, 1=down)
                // Bit 3: Y1 (toggles per Y movement)
                // Bit 7: Button (active low)
                byte val = 0;

                // Process accumulated movement
                if (mouse.delta_x != 0) {
                    val |= 0x01;  // X movement occurred
                    if (mouse.delta_x > 0) {
                        val |= 0x02;  // Moving right
                        mouse.delta_x--;
                    } else {
                        mouse.delta_x++;
                    }
                }
                if (mouse.delta_y != 0) {
                    val |= 0x08;  // Y movement occurred
                    if (mouse.delta_y > 0) {
                        val |= 0x04;  // Moving down
                        mouse.delta_y--;
                    } else {
                        mouse.delta_y++;
                    }
                }

                // Button (active low)
                if (!mouse.button) {
                    val |= 0x80;
                }

                return val;
            } else {
                return mouse.pia_ddra;
            }

        case PIA_CRA:
            return mouse.pia_cra;

        case PIA_ORB:
            if (mouse.pia_crb & 0x04) {
                return mouse.pia_orb;
            } else {
                return mouse.pia_ddrb;
            }

        case PIA_CRB:
            return mouse.pia_crb;
    }
    return 0;
}

static void pia_write(int reg, byte val)
{
    switch (reg) {
        case PIA_ORA:
            if (mouse.pia_cra & 0x04) {
                mouse.pia_ora = val;
            } else {
                mouse.pia_ddra = val;
            }
            break;

        case PIA_CRA:
            mouse.pia_cra = val;
            break;

        case PIA_ORB:
            if (mouse.pia_crb & 0x04) {
                mouse.pia_orb = val;
                DEBUG("Mouse: ROM page = %d\n", get_rom_page());
            } else {
                mouse.pia_ddrb = val;
            }
            break;

        case PIA_CRB:
            mouse.pia_crb = val;
            break;
    }
}

//=============================================================================
// Peripheral handler
//=============================================================================

static byte handler(word loc, int val, int ploc, int psw)
{
    // Handle slot ROM reads ($Cn00-$CnFF)
    if (psw == -1 && ploc >= 0) {
        byte page = get_rom_page();
        word rom_offset = (page * MOUSE_PAGE_SIZE) + ploc;

        if (rom_offset < MOUSE_ROM_SIZE) {
            return mouse_rom[rom_offset];
        }
        return 0x00;
    }

    // Handle soft switch I/O ($C0n0-$C0nF)
    if (psw >= 0 && psw < 4) {
        if (val < 0) {
            // Read
            return pia_read(psw);
        } else {
            // Write
            pia_write(psw, (byte)val);
            return 0;
        }
    }

    return 0;
}

//=============================================================================
// Initialization
//=============================================================================

static void init(void)
{
    fprintf(stderr, "Mouse: Initializing AppleMouse in slot %d\n", slot_num);

    // Reset mouse state
    memset(&mouse, 0, sizeof(mouse));
    mouse.x = 512;  // Start centered
    mouse.y = 512;

    // Load the ROM
    load_mouse_rom();
}

// Public configuration functions
void mouse_set_slot(unsigned int slot)
{
    if (slot >= 1 && slot <= 7) {
        slot_num = slot;
    }
}

unsigned int mouse_get_slot(void)
{
    return slot_num;
}

PeriphDesc mousecard = {
    init,
    handler,
};
