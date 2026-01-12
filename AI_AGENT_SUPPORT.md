# Bobbin AI Agent Support

This document describes modifications to Bobbin that enable reliable integration with AI coding agents.

## Problem

When AI agents (like Claude) control Bobbin via pexpect, keyboard input can be unreliable:
- Characters sent via stdin may arrive faster than GETLN can process them
- Timing issues between character arrival and keyboard polling
- Race conditions when multiple characters are buffered

## Solution: Keyboard Injection Queue

A dedicated keyboard injection queue has been added that:
1. Takes priority over stdin input
2. Is not affected by timing issues
3. Properly integrates with Apple II keyboard polling

### How It Works

1. AI agent enters debugger (Ctrl-C twice)
2. Uses `keys` command to inject keystrokes
3. Continues emulation
4. Apple II GETLN reads characters from injection queue

### Debugger Command

```
keys TEXT
    Inject TEXT as keyboard input (for AI agents).
    Escape sequences: \r=RETURN, \n=RETURN, \e=ESC.
```

Examples:
```
> keys HELLO\r
Injected 6 characters.
> keys 192.168.1.100\r
Injected 15 characters.
```

### MCP Integration

The MCP server's `type_text` method now uses keyboard injection by default:

```python
# Reliable injection (default)
emu.type_text("HELLO WORLD", include_return=True)

# Or explicitly
emu.inject_keys("HELLO WORLD", include_return=True)

# Legacy stdin-based (may have timing issues)
emu.type_text("HELLO WORLD", include_return=True, use_inject=False)
```

## Technical Details

### Source Files Modified

- `src/interfaces/simple.c`: Added injection queue and integration with read_char()/consume_char()
- `src/cmd.c`: Added `keys` debugger command
- `src/bobbin-internal.h`: Added function declaration

### Queue Implementation

The injection queue is a circular buffer that:
- Holds up to 1024 characters
- Uses peek/consume pattern (like the existing linebuf)
- Integrates with the existing keyboard polling mechanism

When $C000 is read:
1. Check injection queue first
2. If queue has characters, return next character (with high bit set)
3. Otherwise, fall back to stdin input

When $C010 is accessed (strobe clear):
1. If last read was from injection queue, consume that character
2. Otherwise, use existing linebuf consumption logic

## Building

```bash
cd bobbin
make
```

## Testing

Test keyboard injection manually:
```bash
./bobbin --simple -m enhanced
# Wait for BASIC prompt
# Press Ctrl-C twice to enter debugger
> keys 10 PRINT "HELLO"\r
> keys 20 GOTO 10\r
> c
# Press Ctrl-C twice again
> keys RUN\r
> c
```
