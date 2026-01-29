//  control_socket.c
//
//  Unix socket control interface for AI/MCP integration.
//  Provides a JSON-based command protocol that bypasses terminal I/O.
//
//  Copyright (c) 2025 Apple II Agent Project.
//  This code is licensed under the MIT license.

#include "bobbin-internal.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

// Control socket state
static int server_fd = -1;
static int client_fd = -1;
static char *socket_path = NULL;
static char cmd_buffer[8192];
static size_t cmd_buffer_len = 0;

// Response buffer (large enough for graphics data)
static char resp_buffer[262144];

// Breakpoint management (mirrors debug.c structures)
#define MAX_BREAKPOINTS 64
typedef struct {
    word addr;
    bool enabled;
    bool is_watch;
    byte last_val;  // For watchpoints
} CtlBreakpoint;

static CtlBreakpoint breakpoints[MAX_BREAKPOINTS];
static int num_breakpoints = 0;

// Speed control
static bool turbo_override = false;
static bool paused = false;
static bool single_step_mode = false;

// Forward declarations
static void handle_command(const char *cmd, size_t len);
static void send_response(const char *resp);
static void send_error(const char *msg);
static void send_ok(void);

// Screen decoding (from simple.c's vidout logic)
static int screen_char_to_ascii(byte c) {
    // Apple II screen codes to ASCII
    if (c >= 0xA0 && c <= 0xDF) {
        return c & 0x7F;  // Normal text
    } else if (c >= 0x80 && c <= 0x9F) {
        return (c & 0x7F) + 0x40;  // Inverse uppercase
    } else if (c >= 0x00 && c <= 0x1F) {
        return c + 0x40;  // Inverse symbols
    } else if (c >= 0x20 && c <= 0x3F) {
        return c;  // Inverse numbers/symbols
    } else if (c >= 0x40 && c <= 0x5F) {
        return c;  // Flash uppercase
    } else if (c >= 0x60 && c <= 0x7F) {
        return c - 0x20;  // Flash symbols -> normal
    } else if (c >= 0xE0 && c <= 0xFF) {
        return c & 0x7F;  // Lowercase (MouseText region on //e)
    }
    return ' ';  // Unknown -> space
}

// Initialize control socket
int control_socket_init(const char *path) {
    fprintf(stderr, "[control_socket] init called with path: %s\n", path ? path : "(null)");
    if (path == NULL) return 0;  // Disabled

    socket_path = strdup(path);
    if (!socket_path) {
        WARN("control_socket: strdup failed\n");
        return -1;
    }

    // Remove existing socket file
    unlink(path);

    // Create Unix domain socket
    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        WARN("control_socket: socket() failed: %s\n", strerror(errno));
        return -1;
    }

    // Set non-blocking
    int flags = fcntl(server_fd, F_GETFL, 0);
    fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);

    // Bind to path
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        WARN("control_socket: bind() failed: %s\n", strerror(errno));
        close(server_fd);
        server_fd = -1;
        return -1;
    }

    // Listen for connections
    if (listen(server_fd, 1) < 0) {
        WARN("control_socket: listen() failed: %s\n", strerror(errno));
        close(server_fd);
        server_fd = -1;
        return -1;
    }

    INFO("Control socket listening on %s\n", path);
    return 0;
}

// Cleanup control socket
void control_socket_cleanup(void) {
    if (client_fd >= 0) {
        close(client_fd);
        client_fd = -1;
    }
    if (server_fd >= 0) {
        close(server_fd);
        server_fd = -1;
    }
    if (socket_path) {
        unlink(socket_path);
        free(socket_path);
        socket_path = NULL;
    }
}

// Check breakpoints - called from main loop
bool control_socket_check_breakpoints(void) {
    if (paused) return true;

    word pc = current_pc();

    for (int i = 0; i < num_breakpoints; i++) {
        if (!breakpoints[i].enabled) continue;

        if (breakpoints[i].is_watch) {
            // Watchpoint - check if value changed
            byte cur_val = peek_sneaky(breakpoints[i].addr);
            if (cur_val != breakpoints[i].last_val) {
                breakpoints[i].last_val = cur_val;
                paused = true;
                return true;
            }
        } else {
            // Breakpoint - check PC
            if (pc == breakpoints[i].addr) {
                paused = true;
                return true;
            }
        }
    }

    if (single_step_mode) {
        single_step_mode = false;
        paused = true;
        return true;
    }

    return false;
}

// Poll for socket activity (call from main loop)
void control_socket_poll(void) {
    if (server_fd < 0) return;

    // Accept new connections
    if (client_fd < 0) {
        client_fd = accept(server_fd, NULL, NULL);
        if (client_fd >= 0) {
            // Set non-blocking
            int flags = fcntl(client_fd, F_GETFL, 0);
            fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
            INFO("Control socket: client connected\n");
            cmd_buffer_len = 0;
        }
    }

    if (client_fd < 0) return;

    // Read available data
    char buf[1024];
    ssize_t n = read(client_fd, buf, sizeof(buf));

    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            WARN("Control socket: read error: %s\n", strerror(errno));
            close(client_fd);
            client_fd = -1;
        }
        return;
    }

    if (n == 0) {
        INFO("Control socket: client disconnected\n");
        close(client_fd);
        client_fd = -1;
        return;
    }

    // Append to command buffer
    for (ssize_t i = 0; i < n; i++) {
        if (cmd_buffer_len < sizeof(cmd_buffer) - 1) {
            cmd_buffer[cmd_buffer_len++] = buf[i];
        }

        // Check for newline (command delimiter)
        if (buf[i] == '\n') {
            cmd_buffer[cmd_buffer_len - 1] = '\0';
            handle_command(cmd_buffer, cmd_buffer_len - 1);
            cmd_buffer_len = 0;
        }
    }
}

// Send response to client
static void send_response(const char *resp) {
    if (client_fd < 0) return;

    size_t len = strlen(resp);
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = write(client_fd, resp + sent, len - sent);
        if (n <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            WARN("Control socket: write error: %s\n", strerror(errno));
            close(client_fd);
            client_fd = -1;
            return;
        }
        sent += n;
    }

    write(client_fd, "\n", 1);
}

static void send_error(const char *msg) {
    snprintf(resp_buffer, sizeof(resp_buffer), "{\"error\":\"%s\"}", msg);
    send_response(resp_buffer);
}

static void send_ok(void) {
    send_response("{\"ok\":true}");
}

// Simple JSON string extraction (no full parser needed)
static bool json_get_string(const char *json, const char *key, char *out, size_t out_sz) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);

    const char *p = strstr(json, search);
    if (!p) return false;

    p += strlen(search);
    while (*p == ' ' || *p == '\t') p++;

    if (*p != '"') return false;
    p++;

    size_t i = 0;
    while (*p && *p != '"' && i < out_sz - 1) {
        if (*p == '\\' && *(p+1)) {
            p++;
            switch (*p) {
                case 'n': out[i++] = '\n'; break;
                case 'r': out[i++] = '\r'; break;
                case 't': out[i++] = '\t'; break;
                case '\\': out[i++] = '\\'; break;
                case '"': out[i++] = '"'; break;
                default: out[i++] = *p; break;
            }
        } else {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    return true;
}

static bool json_get_int(const char *json, const char *key, int *out) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);

    const char *p = strstr(json, search);
    if (!p) return false;

    p += strlen(search);
    while (*p == ' ' || *p == '\t') p++;

    char *end;
    long val = strtol(p, &end, 0);
    if (end == p) return false;

    *out = (int)val;
    return true;
}

static bool json_get_bool(const char *json, const char *key, bool *out) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\":", key);

    const char *p = strstr(json, search);
    if (!p) return false;

    p += strlen(search);
    while (*p == ' ' || *p == '\t') p++;

    if (strncmp(p, "true", 4) == 0) {
        *out = true;
        return true;
    } else if (strncmp(p, "false", 5) == 0) {
        *out = false;
        return true;
    }
    return false;
}

// ============================================================================
// Graphics capture helpers
// ============================================================================

// HGR line address calculation
static word hgr_line_addr(int line, word base) {
    int group = line % 8;
    int third = line / 64;
    int row_in_group = (line / 8) % 8;
    return base + (group * 0x400) + (third * 0x28) + (row_in_group * 0x80);
}

// Text screen line addresses
static const word text_line_addrs[24] = {
    0x400, 0x480, 0x500, 0x580, 0x600, 0x680, 0x700, 0x780,
    0x428, 0x4A8, 0x528, 0x5A8, 0x628, 0x6A8, 0x728, 0x7A8,
    0x450, 0x4D0, 0x550, 0x5D0, 0x650, 0x6D0, 0x750, 0x7D0
};

// GR (Lo-Res) colors RGB
static const byte gr_colors[16][3] = {
    {0, 0, 0},       // 0: Black
    {227, 30, 96},   // 1: Magenta/Red
    {96, 78, 189},   // 2: Dark Blue
    {255, 68, 253},  // 3: Purple/Violet
    {0, 163, 96},    // 4: Dark Green
    {156, 156, 156}, // 5: Grey 1
    {20, 207, 253},  // 6: Medium Blue
    {208, 195, 255}, // 7: Light Blue
    {96, 114, 3},    // 8: Brown
    {255, 106, 60},  // 9: Orange
    {156, 156, 156}, // 10: Grey 2
    {255, 160, 208}, // 11: Pink
    {20, 245, 60},   // 12: Green
    {208, 221, 141}, // 13: Yellow
    {114, 255, 208}, // 14: Aqua/Cyan
    {255, 255, 255}, // 15: White
};

// ============================================================================
// Command handler
// ============================================================================

static void handle_command(const char *cmd, size_t len) {
    char cmd_type[32] = {0};

    if (!json_get_string(cmd, "cmd", cmd_type, sizeof(cmd_type))) {
        send_error("missing cmd field");
        return;
    }

    // =========================================================================
    // PING - Health check and version info
    // =========================================================================
    if (strcmp(cmd_type, "ping") == 0) {
        snprintf(resp_buffer, sizeof(resp_buffer),
                 "{\"ok\":true,\"version\":\"2.0.0\",\"machine\":\"%s\",\"paused\":%s}",
                 cfg.machine ? cfg.machine : "unknown",
                 paused ? "true" : "false");
        send_response(resp_buffer);
    }

    // =========================================================================
    // PEEK - Read memory
    // =========================================================================
    else if (strcmp(cmd_type, "peek") == 0) {
        int addr = 0, length = 1;
        if (!json_get_int(cmd, "addr", &addr)) {
            send_error("missing addr");
            return;
        }
        json_get_int(cmd, "len", &length);

        if (addr < 0 || addr > 0xFFFF) {
            send_error("addr out of range");
            return;
        }
        if (length < 1 || length > 4096) {
            send_error("len out of range (1-4096)");
            return;
        }
        if (addr + length > 0x10000) {
            length = 0x10000 - addr;
        }

        char *p = resp_buffer;
        p += sprintf(p, "{\"ok\":true,\"data\":[");

        for (int i = 0; i < length; i++) {
            if (i > 0) *p++ = ',';
            p += sprintf(p, "%d", peek_sneaky(addr + i));
        }

        strcpy(p, "]}");
        send_response(resp_buffer);
    }

    // =========================================================================
    // POKE - Write memory
    // =========================================================================
    else if (strcmp(cmd_type, "poke") == 0) {
        int addr = 0;
        if (!json_get_int(cmd, "addr", &addr)) {
            send_error("missing addr");
            return;
        }

        if (addr < 0 || addr > 0xFFFF) {
            send_error("addr out of range");
            return;
        }

        const char *data_start = strstr(cmd, "\"data\":");
        if (!data_start) {
            send_error("missing data array");
            return;
        }

        data_start = strchr(data_start, '[');
        if (!data_start) {
            send_error("invalid data format");
            return;
        }
        data_start++;

        int count = 0;
        while (*data_start && *data_start != ']') {
            while (*data_start == ' ' || *data_start == ',' || *data_start == '\t') {
                data_start++;
            }
            if (*data_start == ']') break;

            char *end;
            long val = strtol(data_start, &end, 0);
            if (end == data_start) break;

            if (addr + count <= 0xFFFF) {
                poke_sneaky(addr + count, val & 0xFF);
                count++;
            }
            data_start = end;
        }

        snprintf(resp_buffer, sizeof(resp_buffer), "{\"ok\":true,\"count\":%d}", count);
        send_response(resp_buffer);
    }

    // =========================================================================
    // SCREEN - Read text screen as decoded lines
    // =========================================================================
    else if (strcmp(cmd_type, "screen") == 0) {
        char *p = resp_buffer;
        p += sprintf(p, "{\"ok\":true,\"lines\":[");

        for (int row = 0; row < 24; row++) {
            if (row > 0) *p++ = ',';
            *p++ = '"';

            for (int col = 0; col < 40; col++) {
                byte c = peek_sneaky(text_line_addrs[row] + col);
                int ascii = screen_char_to_ascii(c);

                if (ascii == '"') {
                    *p++ = '\\';
                    *p++ = '"';
                } else if (ascii == '\\') {
                    *p++ = '\\';
                    *p++ = '\\';
                } else if (ascii >= 0x20 && ascii < 0x7F) {
                    *p++ = ascii;
                } else {
                    *p++ = ' ';
                }
            }

            *p++ = '"';
        }

        strcpy(p, "]}");
        send_response(resp_buffer);
    }

    // =========================================================================
    // SCREEN_RAW - Read raw screen memory $0400-$07FF
    // =========================================================================
    else if (strcmp(cmd_type, "screen_raw") == 0) {
        char *p = resp_buffer;
        p += sprintf(p, "{\"ok\":true,\"data\":[");

        for (int i = 0; i < 0x400; i++) {
            if (i > 0) *p++ = ',';
            p += sprintf(p, "%d", peek_sneaky(0x400 + i));
        }

        strcpy(p, "]}");
        send_response(resp_buffer);
    }

    // =========================================================================
    // KEYS - Inject keystrokes
    // =========================================================================
    else if (strcmp(cmd_type, "keys") == 0) {
        char text[1024] = {0};
        if (!json_get_string(cmd, "text", text, sizeof(text))) {
            send_error("missing text");
            return;
        }

        size_t text_len = strlen(text);
        simple_inject_keys(text, text_len);

        snprintf(resp_buffer, sizeof(resp_buffer), "{\"ok\":true,\"injected\":%zu}", text_len);
        send_response(resp_buffer);
    }

    // =========================================================================
    // CPU - Get CPU register state
    // =========================================================================
    else if (strcmp(cmd_type, "cpu") == 0) {
        snprintf(resp_buffer, sizeof(resp_buffer),
                 "{\"ok\":true,\"pc\":%d,\"a\":%d,\"x\":%d,\"y\":%d,\"sp\":%d,\"p\":%d,"
                 "\"cycles\":%" PRIuMAX ",\"instructions\":%" PRIuMAX ",\"frames\":%" PRIuMAX "}",
                 PC, ACC, XREG, YREG, SP, PFLAGS,
                 cycle_count, instr_count, frame_count);
        send_response(resp_buffer);
    }

    // =========================================================================
    // RESET - Reset CPU (warm or cold)
    // =========================================================================
    else if (strcmp(cmd_type, "reset") == 0) {
        bool cold = false;
        json_get_bool(cmd, "cold", &cold);

        if (cold) {
            mem_reboot();
        }
        cpu_reset();
        event_fire(EV_RESET);
        paused = false;

        send_ok();
    }

    // =========================================================================
    // PAUSE - Pause emulation
    // =========================================================================
    else if (strcmp(cmd_type, "pause") == 0) {
        paused = true;
        send_ok();
    }

    // =========================================================================
    // RESUME - Resume emulation
    // =========================================================================
    else if (strcmp(cmd_type, "resume") == 0) {
        paused = false;
        single_step_mode = false;
        send_ok();
    }

    // =========================================================================
    // STEP - Single-step one instruction
    // =========================================================================
    else if (strcmp(cmd_type, "step") == 0) {
        int count = 1;
        json_get_int(cmd, "count", &count);
        if (count < 1) count = 1;
        if (count > 1000) count = 1000;

        // Execute instructions
        for (int i = 0; i < count; i++) {
            cpu_step();
        }

        // Return new CPU state
        snprintf(resp_buffer, sizeof(resp_buffer),
                 "{\"ok\":true,\"pc\":%d,\"a\":%d,\"x\":%d,\"y\":%d,\"sp\":%d,\"p\":%d}",
                 PC, ACC, XREG, YREG, SP, PFLAGS);
        send_response(resp_buffer);
    }

    // =========================================================================
    // BREAK_SET - Set a breakpoint
    // =========================================================================
    else if (strcmp(cmd_type, "break_set") == 0) {
        int addr = 0;
        if (!json_get_int(cmd, "addr", &addr)) {
            send_error("missing addr");
            return;
        }

        if (addr < 0 || addr > 0xFFFF) {
            send_error("addr out of range");
            return;
        }

        if (num_breakpoints >= MAX_BREAKPOINTS) {
            send_error("too many breakpoints");
            return;
        }

        // Check if already exists
        for (int i = 0; i < num_breakpoints; i++) {
            if (breakpoints[i].addr == addr && !breakpoints[i].is_watch) {
                snprintf(resp_buffer, sizeof(resp_buffer),
                         "{\"ok\":true,\"id\":%d,\"exists\":true}", i);
                send_response(resp_buffer);
                return;
            }
        }

        int id = num_breakpoints++;
        breakpoints[id].addr = addr;
        breakpoints[id].enabled = true;
        breakpoints[id].is_watch = false;

        snprintf(resp_buffer, sizeof(resp_buffer), "{\"ok\":true,\"id\":%d}", id);
        send_response(resp_buffer);
    }

    // =========================================================================
    // BREAK_CLEAR - Clear a breakpoint
    // =========================================================================
    else if (strcmp(cmd_type, "break_clear") == 0) {
        int id = -1;
        int addr = -1;
        json_get_int(cmd, "id", &id);
        json_get_int(cmd, "addr", &addr);

        if (id >= 0 && id < num_breakpoints) {
            // Clear by ID - shift remaining breakpoints
            for (int i = id; i < num_breakpoints - 1; i++) {
                breakpoints[i] = breakpoints[i + 1];
            }
            num_breakpoints--;
            send_ok();
        } else if (addr >= 0) {
            // Clear by address
            for (int i = 0; i < num_breakpoints; i++) {
                if (breakpoints[i].addr == addr && !breakpoints[i].is_watch) {
                    for (int j = i; j < num_breakpoints - 1; j++) {
                        breakpoints[j] = breakpoints[j + 1];
                    }
                    num_breakpoints--;
                    send_ok();
                    return;
                }
            }
            send_error("breakpoint not found");
        } else {
            send_error("missing id or addr");
        }
    }

    // =========================================================================
    // BREAK_LIST - List all breakpoints
    // =========================================================================
    else if (strcmp(cmd_type, "break_list") == 0) {
        char *p = resp_buffer;
        p += sprintf(p, "{\"ok\":true,\"breakpoints\":[");

        for (int i = 0; i < num_breakpoints; i++) {
            if (i > 0) *p++ = ',';
            p += sprintf(p, "{\"id\":%d,\"addr\":%d,\"enabled\":%s,\"type\":\"%s\"}",
                         i, breakpoints[i].addr,
                         breakpoints[i].enabled ? "true" : "false",
                         breakpoints[i].is_watch ? "watch" : "break");
        }

        strcpy(p, "]}");
        send_response(resp_buffer);
    }

    // =========================================================================
    // BREAK_ENABLE / BREAK_DISABLE - Enable or disable a breakpoint
    // =========================================================================
    else if (strcmp(cmd_type, "break_enable") == 0 || strcmp(cmd_type, "break_disable") == 0) {
        bool enable = (strcmp(cmd_type, "break_enable") == 0);
        int id = -1;
        if (!json_get_int(cmd, "id", &id)) {
            send_error("missing id");
            return;
        }

        if (id < 0 || id >= num_breakpoints) {
            send_error("invalid breakpoint id");
            return;
        }

        breakpoints[id].enabled = enable;
        if (enable && breakpoints[id].is_watch) {
            // Refresh watchpoint value
            breakpoints[id].last_val = peek_sneaky(breakpoints[id].addr);
        }
        send_ok();
    }

    // =========================================================================
    // WATCH_SET - Set a watchpoint (breaks when memory changes)
    // =========================================================================
    else if (strcmp(cmd_type, "watch_set") == 0) {
        int addr = 0;
        if (!json_get_int(cmd, "addr", &addr)) {
            send_error("missing addr");
            return;
        }

        if (addr < 0 || addr > 0xFFFF) {
            send_error("addr out of range");
            return;
        }

        if (num_breakpoints >= MAX_BREAKPOINTS) {
            send_error("too many breakpoints");
            return;
        }

        int id = num_breakpoints++;
        breakpoints[id].addr = addr;
        breakpoints[id].enabled = true;
        breakpoints[id].is_watch = true;
        breakpoints[id].last_val = peek_sneaky(addr);

        snprintf(resp_buffer, sizeof(resp_buffer),
                 "{\"ok\":true,\"id\":%d,\"current_value\":%d}", id, breakpoints[id].last_val);
        send_response(resp_buffer);
    }

    // =========================================================================
    // LOAD - Load binary data into memory
    // =========================================================================
    else if (strcmp(cmd_type, "load") == 0) {
        int addr = 0;
        char hex[65536] = {0};

        if (!json_get_int(cmd, "addr", &addr)) {
            send_error("missing addr");
            return;
        }
        if (!json_get_string(cmd, "hex", hex, sizeof(hex))) {
            send_error("missing hex data");
            return;
        }

        size_t hex_len = strlen(hex);
        int count = 0;

        for (size_t i = 0; i + 1 < hex_len; i += 2) {
            char byte_str[3] = {hex[i], hex[i+1], 0};
            char *end;
            long val = strtol(byte_str, &end, 16);
            if (end != byte_str + 2) continue;

            if (addr + count <= 0xFFFF) {
                poke_sneaky(addr + count, val & 0xFF);
                count++;
            }
        }

        snprintf(resp_buffer, sizeof(resp_buffer), "{\"ok\":true,\"len\":%d}", count);
        send_response(resp_buffer);
    }

    // =========================================================================
    // DISK_STATUS - Get disk drive status
    // =========================================================================
    else if (strcmp(cmd_type, "disk_status") == 0) {
        int active = active_disk();
        snprintf(resp_buffer, sizeof(resp_buffer),
                 "{\"ok\":true,\"active\":%d,\"spinning\":%s}",
                 active, drive_spinning() ? "true" : "false");
        send_response(resp_buffer);
    }

    // =========================================================================
    // DISK_INSERT - Insert a disk image
    // =========================================================================
    else if (strcmp(cmd_type, "disk_insert") == 0) {
        int drive = 1;
        char path[512] = {0};

        json_get_int(cmd, "drive", &drive);
        if (!json_get_string(cmd, "path", path, sizeof(path))) {
            send_error("missing path");
            return;
        }

        if (drive < 1 || drive > 2) {
            send_error("drive must be 1 or 2");
            return;
        }

        int result = insert_disk(drive, path);
        if (result == 0) {
            send_ok();
        } else {
            send_error("failed to insert disk");
        }
    }

    // =========================================================================
    // DISK_EJECT - Eject a disk
    // =========================================================================
    else if (strcmp(cmd_type, "disk_eject") == 0) {
        int drive = 1;
        json_get_int(cmd, "drive", &drive);

        if (drive < 1 || drive > 2) {
            send_error("drive must be 1 or 2");
            return;
        }

        eject_disk(drive);
        send_ok();
    }

    // =========================================================================
    // SPEED - Set emulation speed
    // =========================================================================
    else if (strcmp(cmd_type, "speed") == 0) {
        bool turbo = false;
        if (json_get_bool(cmd, "turbo", &turbo)) {
            turbo_override = turbo;
            // Note: actual speed change requires integration with main loop
        }

        snprintf(resp_buffer, sizeof(resp_buffer),
                 "{\"ok\":true,\"turbo\":%s}", turbo_override ? "true" : "false");
        send_response(resp_buffer);
    }

    // =========================================================================
    // CYCLES - Get cycle/instruction/frame counts
    // =========================================================================
    else if (strcmp(cmd_type, "cycles") == 0) {
        snprintf(resp_buffer, sizeof(resp_buffer),
                 "{\"ok\":true,\"cycles\":%" PRIuMAX ",\"instructions\":%" PRIuMAX ",\"frames\":%" PRIuMAX "}",
                 cycle_count, instr_count, frame_count);
        send_response(resp_buffer);
    }

    // =========================================================================
    // TRACE - Enable/disable instruction tracing
    // =========================================================================
    else if (strcmp(cmd_type, "trace") == 0) {
        bool enable = false;
        if (json_get_bool(cmd, "enable", &enable)) {
            if (enable) {
                trace_on("Trace started via control socket");
            } else {
                trace_off();
            }
        }

        snprintf(resp_buffer, sizeof(resp_buffer),
                 "{\"ok\":true,\"tracing\":%s,\"file\":\"%s\"}",
                 tracing() ? "true" : "false",
                 cfg.trace_file ? cfg.trace_file : "trace.log");
        send_response(resp_buffer);
    }

    // =========================================================================
    // MOUSE - Control mouse position and button
    // =========================================================================
    else if (strcmp(cmd_type, "mouse") == 0) {
        int x = -1, y = -1;
        bool button = false;
        bool has_button = false;

        json_get_int(cmd, "x", &x);
        json_get_int(cmd, "y", &y);
        has_button = json_get_bool(cmd, "button", &button);

        if (x >= 0 && y >= 0) {
            mouse_set_position(x, y);
        }
        if (has_button) {
            mouse_set_button(button);
        }

        // Get current state
        word mx, my;
        bool mb;
        mouse_get_state(&mx, &my, &mb);

        snprintf(resp_buffer, sizeof(resp_buffer),
                 "{\"ok\":true,\"x\":%d,\"y\":%d,\"button\":%s,\"slot\":%d}",
                 mx, my, mb ? "true" : "false", mouse_get_slot());
        send_response(resp_buffer);
    }

    // =========================================================================
    // SLOTS - Get peripheral slot configuration
    // =========================================================================
    else if (strcmp(cmd_type, "slots") == 0) {
        char *p = resp_buffer;
        p += sprintf(p, "{\"ok\":true,\"slots\":{");

        // Report known peripherals
        p += sprintf(p, "\"3\":\"%s\"", cfg.uthernet2_set ? "uthernet2" : "empty");
        p += sprintf(p, ",\"4\":\"%s\"", cfg.mouse_set ? "mouse" : "empty");
        p += sprintf(p, ",\"5\":\"%s\"", cfg.hdd_set ? "smartport" : "empty");
        p += sprintf(p, ",\"6\":\"disk2\"");  // Disk II always in slot 6

        strcpy(p, "}}");
        send_response(resp_buffer);
    }

    // =========================================================================
    // HGR - Capture HGR graphics to file
    // =========================================================================
    else if (strcmp(cmd_type, "hgr") == 0) {
        int page = 1;
        char path[512] = {0};
        bool color = false;

        json_get_int(cmd, "page", &page);
        json_get_bool(cmd, "color", &color);
        if (!json_get_string(cmd, "path", path, sizeof(path))) {
            send_error("missing path");
            return;
        }

        word base = (page == 2) ? 0x4000 : 0x2000;
        int result = hgr_export_ppm(base, path, color);

        if (result == 0) {
            snprintf(resp_buffer, sizeof(resp_buffer),
                     "{\"ok\":true,\"path\":\"%s\",\"width\":280,\"height\":192}", path);
            send_response(resp_buffer);
        } else {
            send_error("failed to write file");
        }
    }

    // =========================================================================
    // GR - Capture GR (lo-res) graphics to file
    // =========================================================================
    else if (strcmp(cmd_type, "gr") == 0) {
        int page = 1;
        char path[512] = {0};

        json_get_int(cmd, "page", &page);
        if (!json_get_string(cmd, "path", path, sizeof(path))) {
            send_error("missing path");
            return;
        }

        word base = (page == 2) ? 0x0800 : 0x0400;
        int result = gr_export_ppm(base, path);

        if (result == 0) {
            snprintf(resp_buffer, sizeof(resp_buffer),
                     "{\"ok\":true,\"path\":\"%s\",\"width\":40,\"height\":48}", path);
            send_response(resp_buffer);
        } else {
            send_error("failed to write file");
        }
    }

    // =========================================================================
    // DHGR - Capture DHGR (double hi-res) graphics to file
    // =========================================================================
    else if (strcmp(cmd_type, "dhgr") == 0) {
        int page = 1;
        char path[512] = {0};

        json_get_int(cmd, "page", &page);
        if (!json_get_string(cmd, "path", path, sizeof(path))) {
            send_error("missing path");
            return;
        }

        word base = (page == 2) ? 0x4000 : 0x2000;
        int result = dhgr_export_ppm(base, path);

        if (result == 0) {
            snprintf(resp_buffer, sizeof(resp_buffer),
                     "{\"ok\":true,\"path\":\"%s\",\"width\":560,\"height\":192}", path);
            send_response(resp_buffer);
        } else {
            send_error("failed to write file");
        }
    }

    // =========================================================================
    // DGR - Capture DGR (double lo-res) graphics to file
    // =========================================================================
    else if (strcmp(cmd_type, "dgr") == 0) {
        int page = 1;
        char path[512] = {0};

        json_get_int(cmd, "page", &page);
        if (!json_get_string(cmd, "path", path, sizeof(path))) {
            send_error("missing path");
            return;
        }

        word base = (page == 2) ? 0x0800 : 0x0400;
        int result = dgr_export_ppm(base, path);

        if (result == 0) {
            snprintf(resp_buffer, sizeof(resp_buffer),
                     "{\"ok\":true,\"path\":\"%s\",\"width\":80,\"height\":48}", path);
            send_response(resp_buffer);
        } else {
            send_error("failed to write file");
        }
    }

    // =========================================================================
    // SAVE_STATE - Save emulator RAM state
    // =========================================================================
    else if (strcmp(cmd_type, "save_state") == 0) {
        char path[512] = {0};
        if (!json_get_string(cmd, "path", path, sizeof(path))) {
            send_error("missing path");
            return;
        }

        FILE *f = fopen(path, "wb");
        if (!f) {
            send_error("failed to open file");
            return;
        }

        // Write header
        const char *header = "BOBBIN_STATE_V1\n";
        fwrite(header, 1, strlen(header), f);

        // Write CPU state
        fwrite(&theCpu.regs, sizeof(theCpu.regs), 1, f);

        // Write main memory (64KB)
        const byte *ram = getram();
        fwrite(ram, 1, 0x10000, f);

        // Write soft switches
        fwrite(&ss, sizeof(ss), 1, f);

        fclose(f);

        snprintf(resp_buffer, sizeof(resp_buffer), "{\"ok\":true,\"path\":\"%s\"}", path);
        send_response(resp_buffer);
    }

    // =========================================================================
    // LOAD_STATE - Load emulator RAM state
    // =========================================================================
    else if (strcmp(cmd_type, "load_state") == 0) {
        char path[512] = {0};
        if (!json_get_string(cmd, "path", path, sizeof(path))) {
            send_error("missing path");
            return;
        }

        FILE *f = fopen(path, "rb");
        if (!f) {
            send_error("failed to open file");
            return;
        }

        // Read and verify header
        char header[20] = {0};
        if (fread(header, 1, 16, f) != 16 || strncmp(header, "BOBBIN_STATE_V1", 15) != 0) {
            fclose(f);
            send_error("invalid state file");
            return;
        }

        // Read CPU state
        Registers regs;
        if (fread(&regs, sizeof(regs), 1, f) != 1) {
            fclose(f);
            send_error("failed to read CPU state");
            return;
        }

        // Read memory
        byte ram[0x10000];
        if (fread(ram, 1, 0x10000, f) != 0x10000) {
            fclose(f);
            send_error("failed to read memory");
            return;
        }

        // Read soft switches
        SoftSwitches new_ss;
        if (fread(&new_ss, sizeof(new_ss), 1, f) != 1) {
            fclose(f);
            send_error("failed to read switches");
            return;
        }

        fclose(f);

        // Apply state
        theCpu.regs = regs;
        mem_put(ram, 0, 0x10000);
        memcpy(&ss, &new_ss, sizeof(ss));

        // Trigger display refresh
        event_fire(EV_DISPLAY_TOUCH);

        snprintf(resp_buffer, sizeof(resp_buffer), "{\"ok\":true,\"path\":\"%s\"}", path);
        send_response(resp_buffer);
    }

    // =========================================================================
    // DISASM - Disassemble memory
    // =========================================================================
    else if (strcmp(cmd_type, "disasm") == 0) {
        int addr = PC;
        int count = 16;
        json_get_int(cmd, "addr", &addr);
        json_get_int(cmd, "count", &count);

        if (count < 1) count = 1;
        if (count > 64) count = 64;

        char *p = resp_buffer;
        p += sprintf(p, "{\"ok\":true,\"lines\":[");

        word pos = addr;
        for (int i = 0; i < count; i++) {
            if (i > 0) *p++ = ',';

            // Get the opcode
            byte op = peek_sneaky(pos);
            word start_pos = pos;

            // Simple disassembly - just show hex bytes for now
            // A full disassembler would decode the instruction
            p += sprintf(p, "{\"addr\":%d,\"bytes\":[%d", start_pos, op);

            // Determine instruction length (simplified)
            int len = 1;
            // ... full opcode length table would go here
            // For now, just show 1-3 bytes
            if (op == 0x20 || op == 0x4C || op == 0xAD || op == 0x8D) len = 3;  // JSR, JMP, LDA abs, STA abs
            else if ((op & 0x0F) == 0x09 || (op & 0x0F) == 0x05) len = 2;  // Immediate, ZP

            for (int j = 1; j < len && pos + j <= 0xFFFF; j++) {
                p += sprintf(p, ",%d", peek_sneaky(pos + j));
            }
            pos += len;

            p += sprintf(p, "]}");
        }

        strcpy(p, "]}");
        send_response(resp_buffer);
    }

    // =========================================================================
    // CALL - Execute from address (JSR equivalent)
    // =========================================================================
    else if (strcmp(cmd_type, "call") == 0) {
        int addr = 0;
        if (!json_get_int(cmd, "addr", &addr)) {
            send_error("missing addr");
            return;
        }

        if (addr < 0 || addr > 0xFFFF) {
            send_error("addr out of range");
            return;
        }

        // Push return address (current PC - 1 for RTS)
        word ret = PC - 1;
        stack_push_sneaky(HI(ret));
        stack_push_sneaky(LO(ret));

        // Jump to address
        PC = addr;

        snprintf(resp_buffer, sizeof(resp_buffer),
                 "{\"ok\":true,\"pc\":%d,\"return\":%d}", PC, ret + 1);
        send_response(resp_buffer);
    }

    // =========================================================================
    // SOFTSWITCHES - Read soft switch states
    // =========================================================================
    else if (strcmp(cmd_type, "softswitches") == 0) {
        char *p = resp_buffer;
        p += sprintf(p, "{\"ok\":true,\"switches\":{");

        p += sprintf(p, "\"text\":%s", swget(ss, ss_text) ? "true" : "false");
        p += sprintf(p, ",\"mixed\":%s", swget(ss, ss_mixed) ? "true" : "false");
        p += sprintf(p, ",\"page2\":%s", swget(ss, ss_page2) ? "true" : "false");
        p += sprintf(p, ",\"hires\":%s", swget(ss, ss_hires) ? "true" : "false");
        p += sprintf(p, ",\"80col\":%s", swget(ss, ss_eightycol) ? "true" : "false");
        p += sprintf(p, ",\"altcharset\":%s", swget(ss, ss_altcharset) ? "true" : "false");
        p += sprintf(p, ",\"ramrd\":%s", swget(ss, ss_ramrd) ? "true" : "false");
        p += sprintf(p, ",\"ramwrt\":%s", swget(ss, ss_ramwrt) ? "true" : "false");
        p += sprintf(p, ",\"altzp\":%s", swget(ss, ss_altzp) ? "true" : "false");
        p += sprintf(p, ",\"80store\":%s", swget(ss, ss_eightystore) ? "true" : "false");

        strcpy(p, "}}");
        send_response(resp_buffer);
    }

    // =========================================================================
    // QUIT - Exit emulator
    // =========================================================================
    else if (strcmp(cmd_type, "quit") == 0) {
        send_ok();
        control_socket_cleanup();
        exit(0);
    }

    // =========================================================================
    // UNKNOWN COMMAND
    // =========================================================================
    else {
        char err[64];
        snprintf(err, sizeof(err), "unknown command: %s", cmd_type);
        send_error(err);
    }
}

// Query if turbo is requested via socket
bool control_socket_turbo(void) {
    return turbo_override;
}

// Query if paused via socket
bool control_socket_paused(void) {
    return paused;
}
