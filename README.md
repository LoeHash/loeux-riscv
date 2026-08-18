# 🧵 loeux — Loe's Unix

**A hobby RISC-V kernel that actually runs.**

---

## 🧠 What is loeux?

loeux is a from‑scratch operating system kernel for the RISC-V architecture.  
It is built for fun, for learning, and for the sheer joy of making something that *works*.

The name? **Loe** + **Unix** = **loeux**.  
Because every great system starts with a name that means something to its creator.

---

## 📖 The Story of loeux

It started in the summer of 2026 — the break between freshman and sophomore year.

I had spent weeks fighting with xv6, trying to implement copy‑on‑write and semaphores.  
I failed. Hard.

One commit from that time says it all:

> *“i closed the c.o.w. ... fuck it! i've spend a week for the bullshit semaphore thing, i will fix it! besides, if you extremly want to use cow, go ahead ... this is very dangerous”*

That was June 12.

I took a step back. I realized I was fighting a system I didn't fully own. So I decided to build my own — from scratch — on RISC-V, where there was no legacy, no 32‑bit cruft, and no borrowed code I didn't understand.

On July 24, I wrote the first commit:

> *`<init> init....`*

25 days later, on August 17, loeux ran its first user program.

> *`<feat> we are finally running the user program! the kernel executing stream and exception controling stream had been verified! enjoy it!`*

It wasn't a breakthrough. It was just one commit after another, day after day — until suddenly, it worked.

That's the story of loeux.  
Not a grand plan. Just a lot of small steps that added up to something real.

---

## ✨ Features

- 🧩 **Multi‑core boot** — Brings up all available RISC-V cores.
- 🧠 **Virtual memory** — Sv39 page tables with full kernel/user space separation.
- ⏱️ **Preemptive scheduling** — Round‑robin scheduler with timer interrupts (CLINT).
- 💾 **Block I/O** — VirtIO block device driver (`virtio_blk`) with synchronous read/write.
- 📁 **FAT12 filesystem** — Complete FAT12 support with a full VFS abstraction layer (`open`, `read`, `write`, `close`, `mount`).
- 📟 **Standard I/O** — `stdin`, `stdout`, `stderr` working through the VFS layer.
- 👤 **User mode** — Can execute user programs and handle system calls.
- 🔧 **Debugging tools** — `vmprint` for page table visualization, built‑in test suites.

---

## 🚀 Build & Run

```bash
make
make qemu
```

This will build the kernel and launch it in QEMU with a 64 MB FAT12 disk image.

---

## 📂 Project Structure

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

## 📊 Current Status

- [x] Multi‑core boot  
- [x] Sv39 virtual memory  
- [x] Process scheduler  
- [x] Timer interrupts  
- [x] VirtIO block driver  
- [x] FAT12 + VFS  
- [x] `stdin` / `stdout` / `stderr`  
- [x] User programs  
- [ ] Copy‑on‑Write (coming soon)  
- [ ] Slab allocator (planned)  
- [ ] More system calls (planned)

---
## 🛠️ Code Stats

```text
-------------------------------------------------------------------------------
Language                     files          blank        comment           code
-------------------------------------------------------------------------------
C                               25            774            822           4091
C/C++ Header                    28            293            521           2008
make                             8             72             18            241
Assembly                         6             33             78            218
Text                             1             91              0            206
JSON                             1              0              0             36
Linker Script                    1              2              0             14
Markdown                         1              0              0              3
-------------------------------------------------------------------------------
SUM:                            71           1265           1439           6817
-------------------------------------------------------------------------------
```

**~6800 lines** of C, assembly, and headers.  
**~1400 lines** of comments — because future me will thank past me.

---

## 📜 License

MIT License — do whatever you want with it, just keep my name somewhere.

---

**LoeHash** · Summer 2026 · 🧵 from `init....` to `sret`