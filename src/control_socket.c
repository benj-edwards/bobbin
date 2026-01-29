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

// Response buffer
static char resp_buffer[65536];

// Forward declarations
static void handle_command(const char *cmd, size_t len);
static void send_response(const char *resp);
static void send_error(const char *msg);
static void send_ok(void);

// Screen decoding (from simple.c's vidout logic)
static int screen_char_to_ascii(byte c) {
    // Apple II screen codes to ASCII
    // Normal: $A0-$DF -> space to underscore (clear high bit)
    // Inverse: $00-$3F -> @-_ (add $40, show as inverse)
    // Flash: $40-$7F -> @-_ (show as flash)

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
            // Real error or disconnect
            WARN("Control socket: read error: %s\n", strerror(errno));
            close(client_fd);
            client_fd = -1;
        }
        return;
    }

    if (n == 0) {
        // Client disconnected
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
                // Would block, try again
                continue;
            }
            WARN("Control socket: write error: %s\n", strerror(errno));
            close(client_fd);
            client_fd = -1;
            return;
        }
        sent += n;
    }

    // Send newline
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

// Handle incoming command
static void handle_command(const char *cmd, size_t len) {
    char cmd_type[32] = {0};

    if (!json_get_string(cmd, "cmd", cmd_type, sizeof(cmd_type))) {
        send_error("missing cmd field");
        return;
    }

    // --- PING ---
    if (strcmp(cmd_type, "ping") == 0) {
        snprintf(resp_buffer, sizeof(resp_buffer),
                 "{\"ok\":true,\"version\":\"1.0.0\",\"machine\":\"%s\"}",
                 cfg.machine ? cfg.machine : "unknown");
        send_response(resp_buffer);
    }

    // --- PEEK ---
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

        // Build response with data array
        char *p = resp_buffer;
        p += sprintf(p, "{\"ok\":true,\"data\":[");

        for (int i = 0; i < length; i++) {
            if (i > 0) *p++ = ',';
            p += sprintf(p, "%d", peek_sneaky(addr + i));
        }

        strcpy(p, "]}");
        send_response(resp_buffer);
    }

    // --- POKE ---
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

        // Find data array
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

        // Parse and poke each byte
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

    // --- SCREEN ---
    else if (strcmp(cmd_type, "screen") == 0) {
        // Read text screen memory and decode
        char *p = resp_buffer;
        p += sprintf(p, "{\"ok\":true,\"lines\":[");

        // Text screen line addresses (Apple II's weird memory layout)
        static const word line_addrs[24] = {
            0x400, 0x480, 0x500, 0x580, 0x600, 0x680, 0x700, 0x780,
            0x428, 0x4A8, 0x528, 0x5A8, 0x628, 0x6A8, 0x728, 0x7A8,
            0x450, 0x4D0, 0x550, 0x5D0, 0x650, 0x6D0, 0x750, 0x7D0
        };

        for (int row = 0; row < 24; row++) {
            if (row > 0) *p++ = ',';
            *p++ = '"';

            for (int col = 0; col < 40; col++) {
                byte c = peek_sneaky(line_addrs[row] + col);
                int ascii = screen_char_to_ascii(c);

                // Escape special JSON characters
                if (ascii == '"') {
                    *p++ = '\\';
                    *p++ = '"';
                } else if (ascii == '\\') {
                    *p++ = '\\';
                    *p++ = '\\';
                } else if (ascii >= 0x20 && ascii < 0x7F) {
                    *p++ = ascii;
                } else {
                    *p++ = ' ';  // Non-printable -> space
                }
            }

            *p++ = '"';
        }

        strcpy(p, "]}");
        send_response(resp_buffer);
    }

    // --- SCREEN_RAW ---
    else if (strcmp(cmd_type, "screen_raw") == 0) {
        // Return raw screen memory $0400-$07FF
        char *p = resp_buffer;
        p += sprintf(p, "{\"ok\":true,\"data\":[");

        for (int i = 0; i < 0x400; i++) {
            if (i > 0) *p++ = ',';
            p += sprintf(p, "%d", peek_sneaky(0x400 + i));
        }

        strcpy(p, "]}");
        send_response(resp_buffer);
    }

    // --- KEYS ---
    else if (strcmp(cmd_type, "keys") == 0) {
        char text[1024] = {0};
        if (!json_get_string(cmd, "text", text, sizeof(text))) {
            send_error("missing text");
            return;
        }

        size_t len = strlen(text);
        simple_inject_keys(text, len);

        snprintf(resp_buffer, sizeof(resp_buffer), "{\"ok\":true,\"injected\":%zu}", len);
        send_response(resp_buffer);
    }

    // --- CPU ---
    else if (strcmp(cmd_type, "cpu") == 0) {
        snprintf(resp_buffer, sizeof(resp_buffer),
                 "{\"ok\":true,\"pc\":%d,\"a\":%d,\"x\":%d,\"y\":%d,\"sp\":%d,\"p\":%d}",
                 PC, ACC, XREG, YREG, SP, PFLAGS);
        send_response(resp_buffer);
    }

    // --- RESET ---
    else if (strcmp(cmd_type, "reset") == 0) {
        bool cold = false;
        json_get_bool(cmd, "cold", &cold);

        if (cold) {
            mem_reboot();
        }
        cpu_reset();
        event_fire(EV_RESET);

        send_ok();
    }

    // --- PAUSE ---
    else if (strcmp(cmd_type, "pause") == 0) {
        dbg_on();
        send_ok();
    }

    // --- RESUME ---
    else if (strcmp(cmd_type, "resume") == 0) {
        // Exit debugger by clearing debug state
        // The debugger will naturally exit on next iteration
        send_ok();
    }

    // --- LOAD ---
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

        // Parse hex string and load into memory
        size_t hex_len = strlen(hex);
        int count = 0;

        for (size_t i = 0; i + 1 < hex_len; i += 2) {
            char byte_str[3] = {hex[i], hex[i+1], 0};
            char *end;
            long val = strtol(byte_str, &end, 16);
            if (end != byte_str + 2) continue;  // Invalid hex

            if (addr + count <= 0xFFFF) {
                poke_sneaky(addr + count, val & 0xFF);
                count++;
            }
        }

        snprintf(resp_buffer, sizeof(resp_buffer), "{\"ok\":true,\"len\":%d}", count);
        send_response(resp_buffer);
    }

    // --- DISK_STATUS ---
    else if (strcmp(cmd_type, "disk_status") == 0) {
        int active = active_disk();
        snprintf(resp_buffer, sizeof(resp_buffer),
                 "{\"ok\":true,\"active\":%d,\"spinning\":%s}",
                 active, drive_spinning() ? "true" : "false");
        send_response(resp_buffer);
    }

    // --- DISK_INSERT ---
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

    // --- DISK_EJECT ---
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

    // --- QUIT ---
    else if (strcmp(cmd_type, "quit") == 0) {
        send_ok();
        control_socket_cleanup();
        exit(0);
    }

    // --- UNKNOWN ---
    else {
        char err[64];
        snprintf(err, sizeof(err), "unknown command: %s", cmd_type);
        send_error(err);
    }
}
