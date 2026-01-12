//  cmd.c
//
//  Copyright (c) 2023-2024 Micah John Cowan.
//  This code is licensed under the MIT license.
//  See the accompanying LICENSE file for details.

#include "bobbin-internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

static const char cmd_help[] = "\
h, help\n\
    print this message.\n\
q, quit\n\
    exit bobbin.\n\
r, w\n\
    send reset (warm).\n\
rr\n\
    send COLD reset.\n\
m\n\
    invoke the Apple ][ monitor.\n\
disk NUM { eject | load PATH }.\n\
    Eject or load a disk image.\n\
save-hgr-ascii FILE (sha FILE)\n\
    Save HGR page 1 as ASCII art.\n\
save-hgr-ppm FILE (shp FILE)\n\
    Save HGR page 1 as PPM image (mono).\n\
save-hgr-ppm-color FILE\n\
    Save HGR page 1 as PPM image (color artifacts).\n\
save-hgr2-ascii FILE (sha2 FILE)\n\
    Save HGR page 2 as ASCII art.\n\
save-hgr2-ppm FILE (shp2 FILE)\n\
    Save HGR page 2 as PPM image (mono).\n\
save-hgr2-ppm-color FILE\n\
    Save HGR page 2 as PPM image (color artifacts).\n\
save-gr-ascii FILE (sga FILE)\n\
    Save GR (lo-res) page 1 as ASCII art.\n\
save-gr-ppm FILE (sgp FILE)\n\
    Save GR page 1 as PPM image (scaled 280x192).\n\
save-gr-ppm-native FILE\n\
    Save GR page 1 as PPM image (native 40x48).\n\
save-gr2-ascii FILE (sga2 FILE)\n\
    Save GR page 2 as ASCII art.\n\
save-gr2-ppm FILE (sgp2 FILE)\n\
    Save GR page 2 as PPM image (scaled 280x192).\n\
save-gr2-ppm-native FILE\n\
    Save GR page 2 as PPM image (native 40x48).\n\
save-dhgr-ascii FILE (sdha FILE)\n\
    Save DHGR (double hi-res) page 1 as ASCII art (//e only).\n\
save-dhgr-ppm FILE (sdhp FILE)\n\
    Save DHGR page 1 as PPM image (560x192, //e only).\n\
save-dhgr2-ascii FILE (sdha2 FILE)\n\
    Save DHGR page 2 as ASCII art.\n\
save-dhgr2-ppm FILE (sdhp2 FILE)\n\
    Save DHGR page 2 as PPM image (560x192).\n\
save-dgr-ascii FILE (sdga FILE)\n\
    Save DGR (double lo-res) page 1 as ASCII art (//e only).\n\
save-dgr-ppm FILE (sdgp FILE)\n\
    Save DGR page 1 as PPM image (scaled 560x192, //e only).\n\
save-dgr-ppm-native FILE\n\
    Save DGR page 1 as PPM image (native 80x48).\n\
save-dgr2-ascii FILE (sdga2 FILE)\n\
    Save DGR page 2 as ASCII art.\n\
save-dgr2-ppm FILE (sdgp2 FILE)\n\
    Save DGR page 2 as PPM image (scaled 560x192).\n\
save-dgr2-ppm-native FILE\n\
    Save DGR page 2 as PPM image (native 80x48).\n\
keys TEXT\n\
    Inject TEXT as keyboard input (for AI agents).\n\
    Escape sequences: \\r=RETURN, \\n=RETURN, \\e=ESC.\n\
";

static const char SAVE_RAM_STR[] = "save-ram ";
static const char DISK_STR[] = "disk ";
static const char LOAD_STR[] = "load ";
static const char KEYS_STR[] = "keys ";

bool command_do(const char *line, printer pr)
{
    bool handled = true;
#define HAVE(s) (STREQ(line,(s)))
    if (HAVE("m")) {
        // Swap ourselves out for the built-in Apple II system
        // monitor!
        pr("Switching to monitor.\n");
        // Behave as if it were a BRK.
        // Push stuff to stack...
        stack_push_sneaky(HI(PC));
        stack_push_sneaky(LO(PC));
        stack_push_sneaky(PFLAGS | PMASK(PUNUSED) | PMASK(PBRK));
        go_to(WORD(peek_sneaky(VEC_BRK),peek_sneaky(VEC_BRK+1)));
        // ^ Note: some autostart ROMs have
        // OLDRST instead of BREAK, in VEC_BRK, with the result that
        // PC and the other registers will NOT be printed on entry
        // into the system monitor.
    } else if (HAVE("r") || HAVE("w")) {
        pr("Sending reset.\n");
        event_fire(EV_RESET);
    } else if (HAVE("rr")) {
        pr("Sending COLD reset.\n");
        event_fire(EV_RESET);
        // Hard reset. Invalidate the user reset vector directly
        // (rather than doing open-apple emulation or something)
        // Just copy the high byte of the vector into the
        // "powered up" check; then it can't possibly be the
        // correctly XOR-ed version.
        byte b = peek_sneaky(LOC_SOFTEV+1);
        poke_sneaky(LOC_PWREDUP, b);
    } else if (HAVE("^C")) {
        // XXX in future this will be replaced by a "send" command
        // that can do other things besides just ^C. ^? or ^D for
        // instance
        // Send an interrupt back through to the emulation, and
        // continue.
        sigint_received = 1;
    } else if (HAVE("q") || HAVE("quit")) {
        event_fire(EV_UNHOOK);
        printf("Exiting.\n"); // Don't use pr
        exit(0);
    } else if (HAVE("h") || HAVE("help")) {
        pr("%s", cmd_help);
    } else if (!memcmp(line, SAVE_RAM_STR, sizeof(SAVE_RAM_STR)-1)) {
        // XXX disable if I ever add a "safe mode"
        line += sizeof(SAVE_RAM_STR)-1; // skip to the argument
        while (*line == ' ') ++line;
        errno = 0;
        FILE *ramfile = fopen(line, "w");
        if (ramfile == NULL) {
            pr("ERR: Could not open \"%s\" for writing: %s\n",
               line, strerror(errno));
            goto ramsave_bail;
        }
        errno = 0;
        int err = fwrite(getram(), 1, 128 * 1024, ramfile);
        if (err < 0) {
            pr("ERR: Could not save RAM to \"%s\": %s\n",
               line, strerror(errno));
            goto ramsave_bail;
        }

        pr("Success: saved RAM to file \"%s\".\n", line);
ramsave_bail:
        if (ramfile != NULL) fclose(ramfile);
    } else if (!memcmp(line, DISK_STR, sizeof(DISK_STR)-1)) {
        line += sizeof(DISK_STR)-1; // skip past command
        while (*line == ' ') ++line; // skip WS

        // Parse disk number
        unsigned long drive;
        char *end;
        drive = strtoul(line, &end, 10);
        if (end == line) {
            pr("ERR: missing drive #\n");
            goto disk_bail;
        }
        if (*end != ' ' && *end != '\0') {
            pr("ERR: malformed drive #\n");
            goto disk_bail;
        }
        if (drive != 1 && drive != 2) {
            pr("ERR: disk: drive # must be either 1 or 2.\n");
            goto disk_bail;
        }

        // Check disk activity before proceeding
        if (drive_spinning() && active_disk() == drive) {
            pr("ERR: can't use \"disk\" command on currently-spinning"
               " drive.\n");
            goto disk_bail;
        }

        // skip more WS
        line = end;
        while (*line == ' ') ++line;
        if (*line == '\0') { // No WS to skip?
            pr("ERR: disk: missing subcommand\n");
            goto disk_bail;
        }

        // Find subcommand
        if (HAVE("eject")) {
            (void) eject_disk(drive);
        } else if (!memcmp(line, LOAD_STR, sizeof(LOAD_STR)-1)) {
            // Disable if I ever have a "safe" mode
            line += sizeof(LOAD_STR)-1;
            while (*line == ' ') ++line; // skip WS
            int err = insert_disk(drive, line);
            if (err) {
                pr("ERR: disk: unknown problem inserting disk %s\n", line);
            }
        } else {
            pr("ERR: disk: unknown subcommand %s\n", line);
        }
disk_bail:
        ;
    } else if (!memcmp(line, KEYS_STR, sizeof(KEYS_STR)-1)) {
        // AI Agent keyboard injection
        pr("DEBUG: keys command received\n");
        line += sizeof(KEYS_STR)-1; // skip to the argument
        pr("DEBUG: argument is: '%s'\n", line);
        // Process escape sequences and inject keys
        char buf[1024];
        size_t buflen = 0;
        while (*line && buflen < sizeof(buf) - 1) {
            if (*line == '\\' && *(line+1)) {
                line++;
                switch (*line) {
                    case 'r':
                    case 'n':
                        buf[buflen++] = '\r';  // Apple II CR
                        break;
                    case 'e':
                        buf[buflen++] = '\x1b';  // ESC
                        break;
                    case '\\':
                        buf[buflen++] = '\\';
                        break;
                    default:
                        buf[buflen++] = *line;
                        break;
                }
                line++;
            } else {
                buf[buflen++] = *line++;
            }
        }
        if (buflen > 0) {
            simple_inject_keys(buf, buflen);
            pr("Injected %zu characters.\n", buflen);
        }
    } else {
        handled = false;
    }
#undef HAVE
    return handled;
}
