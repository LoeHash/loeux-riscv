# loeux — Loe's Unix

A from-scratch RISC-V operating system kernel.

---

## Overview

loeux is a hobby operating system kernel for the RISC-V architecture, written from scratch.

The name is **Loe** + **Unix**. The goal is to understand how a real system works, from the first instruction after reset to a user-mode shell.

---

## Origin

This project started in the summer of 2026, after my freshman year.

Before loeux, I spent weeks on xv6 trying to implement copy-on-write and semaphores. I did not get them working. One commit from that period reads:

> *"i closed the c.o.w. ... fuck it! i've spend a week for the bullshit semaphore thing, i will fix it! besides, if you extremly want to use cow, go ahead ... this is very dangerous"*

That was June 12, 2026.

Rather than continue fighting a system I did not fully own, I decided to build one from scratch on RISC-V — no legacy, no 32-bit cruft, no code I did not understand.

On July 24, 2026, I wrote the first commit:

> *`<init> init....`*

25 days later, on August 17, 2026, loeux ran its first user program.

> *`<feat> we are finally running the user program! the kernel executing stream and exception controling stream had been verified! enjoy it!`*

There was no single breakthrough. Just consistent progress, one commit at a time.

---

## Architecture

```
User programs (shell, ls, cat, mkdir, touch, echo, pwd)
        │
        ▼
System call interface (open/read/write/close/mkdir/sbrk/...)
        │
        ▼
VFS layer ──────────────┬──────────────┐
        │               │              │
        ▼               ▼              ▼
      EXT2           FAT12           TTY
        │               │              │
        └───────┬───────┘              │
                ▼                      ▼
        VirtIO block driver      UART / console
                │
                ▼
        Physical memory (alloc_page / free_page)
                │
                ▼
        Slab allocator
```

---

## Features

- **Multi-core boot** — Brings up all available RISC-V harts.
- **Virtual memory** — Sv39 page tables with kernel/user address space separation.
- **Preemptive scheduling** — Round-robin scheduler driven by CLINT timer interrupts.
- **Block I/O** — VirtIO block device driver with synchronous read/write.
- **Filesystems** — FAT12 and EXT2, both behind a common VFS abstraction layer.
- **VFS** — `open`, `read`, `write`, `close`, `mount`, path resolution following POSIX lexical rules.
- **Memory management** — `alloc_page` / `free_page` for physical pages, Slab allocator for small objects, `sbrk` with lazy allocation.
- **TTY layer** — Unified interface for terminal devices.
- **Standard I/O** — `stdin`, `stdout`, `stderr` exposed through the VFS layer.
- **User mode** — Loads and executes user programs; handles system calls.
- **Shell** — A minimal user-space shell, based on [brenns10/lsh](https://github.com/brenns10/lsh).
- **Debugging tools** — `vmprint` for page-table inspection, built-in test suites.

---

## Build & Run

```bash
make
make qemu
```

Builds the kernel and launches it in QEMU with a 64 MB disk image.

Requirements: RISC-V cross-compiler, QEMU, OpenSBI.

---

## Project Structure

```
.
├── asm/            # Assembly: entry, trampoline, context switch
├── boot/           # Boot code for primary and secondary cores
├── drivers/        # Device drivers (VirtIO disk, TTY)
├── fs/             # Filesystems (VFS, FAT12, EXT2)
├── include/        # All headers
├── kernel/         # Core kernel (start, trap, proc, spinlock, printk, syscall)
├── mm/             # Memory management (physical, virtual, paging, slab)
├── test/           # Test routines
├── user/           # User programs and libraries
└── Makefile
```

---

## Status

- [x] Multi-core boot
- [x] Sv39 virtual memory
- [x] Process scheduler
- [x] Timer interrupts
- [x] VirtIO block driver
- [x] VFS abstraction layer
- [x] FAT12 filesystem
- [x] EXT2 filesystem
- [x] TTY layer
- [x] Slab allocator
- [x] `sbrk` with lazy allocation
- [x] `stdin` / `stdout` / `stderr`
- [x] User programs
- [x] Shell
- [ ] Copy-on-Write
- [ ] Network stack
- [ ] Graphics driver

---

## Acknowledgements

This project includes code and ideas from the following sources:

- **[brenns10/lsh](https://github.com/brenns10/lsh)** — A simple shell implementation in C by Stephen Brennan. The user-space shell in loeux is adapted from this project.

---

## License

MIT License. See `LICENSE` for details.

---

**LoeHash** · 2026
