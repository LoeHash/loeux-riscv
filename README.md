# loeux — Loe's Unix

A hobby RISC-V operating system kernel, written from scratch.

---

## What is loeux?

loeux is a from-scratch operating system kernel for the RISC-V architecture.

The name is **Loe** + **Unix**. It is built for learning how a real system works — from the first instruction after reset to a user-mode shell.

---

## Origin

This project started in the summer of 2026, between freshman and sophomore year.

Before loeux, I spent weeks on xv6 trying to implement copy-on-write and semaphores. I did not get them working. One commit from that period reads:

> *“i closed the c.o.w. ... fuck it! i've spend a week for the bullshit semaphore thing, i will fix it! besides, if you extremly want to use cow, go ahead ... this is very dangerous”*

That was June 12, 2026.

Rather than continue fighting a system I did not fully own, I decided to build one from scratch on RISC-V — no legacy, no 32-bit cruft, no code I did not understand.

On July 24, 2026, I wrote the first commit:

> *`<init> init....`*

25 days later, on August 17, 2026, loeux ran its first user program.

> *`<feat> we are finally running the user program! the kernel executing stream and exception controling stream had been verified! enjoy it!`*

There was no single breakthrough. Just consistent progress, one commit at a time.

---

## Features

- **Multi-core boot** — Brings up all available RISC-V harts.
- **Virtual memory** — Sv39 page tables with kernel/user address space separation.
- **Preemptive scheduling** — Round-robin scheduler driven by CLINT timer interrupts.
- **Block I/O** — VirtIO block device driver with synchronous read/write.
- **Filesystem** — FAT12 implementation with a VFS abstraction layer (`open`, `read`, `write`, `close`, `mount`).
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

Builds the kernel and launches it in QEMU with a 64 MB FAT12 disk image.

---

## Project Structure

```
.
├── asm/            # Assembly: entry, trampoline, context switch
├── boot/           # Boot code for primary and secondary cores
├── drivers/        # Device drivers (VirtIO disk)
├── fs/             # Filesystems (FAT12, VFS)
├── include/        # All headers
├── kernel/         # Core kernel (start, trap, proc, spinlock, printk)
├── mm/             # Memory management (physical, virtual, paging)
├── test/           # Test routines
├── user/           # User programs
└── Makefile
```

---

## Status

- [x] Multi-core boot
- [x] Sv39 virtual memory
- [x] Process scheduler
- [x] Timer interrupts
- [x] VirtIO block driver
- [x] FAT12 + VFS
- [x] `stdin` / `stdout` / `stderr`
- [x] User programs
- [x] Shell
- [ ] Copy-on-Write
- [ ] Slab allocator
- [ ] More system calls

---

## Acknowledgements
This project includes code and ideas from the following sources:

- **[brenns10/lsh](https://github.com/brenns10/lsh)** — A simple shell implementation in C by Stephen Brennan. The user-space shell in loeux is adapted from this project.  
---

## Code Stats

```text
-------------------------------------------------------------------------------
Language                     files          blank        comment           code
-------------------------------------------------------------------------------
C                               28           1000           1137           5540
C/C++ Header                    29            345            653           2342
Text                             1            515              0           1371
D                               29              0              0            576
make                             9             82             26            270
Assembly                         6             51             92            241
Markdown                         1             40              0             94
JSON                             1              0              0             36
Linker Script                    1              2              0             14
-------------------------------------------------------------------------------
SUM:                           105           2035           1908          10484
-------------------------------------------------------------------------------
```

Roughly 10000 lines of C, assembly, and headers, with about 1400 lines of comments.

---

## License

MIT License. See `LICENSE` for details.

---

**LoeHash** · 2026 · from `init....` to `sret`
