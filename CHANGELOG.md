# Bobbin Changelog

## [Unreleased] - 2026-01-28

### Added

- **Control Socket Interface** (`control_socket.c`)
  - Unix domain socket for AI/MCP integration at `/tmp/bobbin.sock`
  - Enable with `--control-socket PATH` command line option
  - JSON-based command protocol, newline-delimited responses
  - Bypasses all pexpect/PTY/signal issues that plagued debugger integration

  **Protocol v2.0 Commands:**

  | Category | Commands |
  |----------|----------|
  | Basic | `ping`, `quit` |
  | Memory | `peek`, `poke`, `load`, `screen`, `screen_raw` |
  | CPU | `cpu`, `step`, `reset`, `pause`, `resume`, `call`, `disasm` |
  | Breakpoints | `break_set`, `break_clear`, `break_list`, `break_enable`, `break_disable`, `watch_set` |
  | Disks | `disk_status`, `disk_insert`, `disk_eject` |
  | Graphics | `hgr`, `gr`, `dhgr`, `dgr` (export to PPM files) |
  | State | `save_state`, `load_state` (full emulator snapshots) |
  | System | `mouse`, `slots`, `softswitches`, `speed`, `cycles`, `trace`, `keys` |

  **Example usage:**
  ```bash
  # Start bobbin with control socket
  bobbin --simple --control-socket /tmp/bobbin.sock -m enhanced --disk mydisk.dsk

  # Connect and send commands (Python)
  from apple2_mcp.control_socket import BobbinControlSocket
  sock = BobbinControlSocket('/tmp/bobbin.sock')
  sock.connect()
  print(sock.ping())        # {'ok': True, 'version': '2.0.0', 'machine': 'enhanced'}
  print(sock.read_screen()) # 24 lines of text
  sock.step(100)            # Single-step 100 instructions
  sock.break_set(0x6000)    # Set breakpoint
  ```

### Fixed

- **Memory leak in DSK disk format handler** (`format/dsk.c`)
  - `eject()` incorrectly called `munmap()` on a buffer allocated with `malloc()`, causing undefined behavior and leaking 232KB per disk eject
  - The mmap'd disk image (`dat->realbuf`) was never unmapped, causing an additional 143KB leak
  - Fixed by using `free()` for the malloc'd nibble buffer and `munmap()` for the mmap'd disk image

- **Excessive CPU usage from per-instruction heap allocation** (`event.c`)
  - `event_fire()`, `event_fire_peek()`, `event_fire_poke()`, `event_fire_disk_active()`, and `event_fire_switch()` all allocated Event structs on the heap
  - At ~1 million instructions/second with multiple memory accesses per instruction, this caused millions of unnecessary malloc/free calls per second
  - Fixed by converting all Event allocations to stack allocation, eliminating heap churn in the hot path
