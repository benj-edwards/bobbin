# Bobbin Changelog

## [Unreleased] - 2026-01-25

### Fixed

- **Memory leak in DSK disk format handler** (`format/dsk.c`)
  - `eject()` incorrectly called `munmap()` on a buffer allocated with `malloc()`, causing undefined behavior and leaking 232KB per disk eject
  - The mmap'd disk image (`dat->realbuf`) was never unmapped, causing an additional 143KB leak
  - Fixed by using `free()` for the malloc'd nibble buffer and `munmap()` for the mmap'd disk image

- **Excessive CPU usage from per-instruction heap allocation** (`event.c`)
  - `event_fire()`, `event_fire_peek()`, `event_fire_poke()`, `event_fire_disk_active()`, and `event_fire_switch()` all allocated Event structs on the heap
  - At ~1 million instructions/second with multiple memory accesses per instruction, this caused millions of unnecessary malloc/free calls per second
  - Fixed by converting all Event allocations to stack allocation, eliminating heap churn in the hot path
