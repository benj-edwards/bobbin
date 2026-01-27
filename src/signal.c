//  signal.c
//
//  Copyright (c) 2023-2024 Micah John Cowan.
//  This code is licensed under the MIT license.
//  See the accompanying LICENSE file for details.

#include "bobbin-internal.h"

#include <stdio.h>
#include <signal.h>
#include <stdlib.h>

volatile sig_atomic_t sigint_received = 0;  // Now used for SIGQUIT (Ctrl-\)
volatile sig_atomic_t sigwinch_received = 0;
volatile sig_atomic_t sigalrm_received = 0;

// Handle SIGQUIT (Ctrl-\) for debugger entry - avoids conflict with Ctrl-C
// which Apple II programs may want to receive
void handle_quit(int s)
{
    ++sigint_received;  // Reuse the same counter for debugger triggering
    signal(SIGQUIT, handle_quit);
}

void handle_winch(int s)
{
    sigwinch_received = true;
    signal(SIGWINCH, handle_winch);
}

void handle_alarm(int s)
{
    sigalrm_received = true;
    signal(SIGALRM, handle_alarm);
}

void signals_init(void)
{
    signal(SIGQUIT, handle_quit);  // Ctrl-\ for debugger
    signal(SIGWINCH, handle_winch);
    signal(SIGALRM, handle_alarm);
}

void unhandle_sigint(void)
{
    signal(SIGQUIT, SIG_DFL);
}
