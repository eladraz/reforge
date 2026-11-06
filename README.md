![maintained](http://img.shields.io/badge/status-maintained-brightgreen.png)
[![CI](https://github.com/eladraz/reforge/actions/workflows/ci.yml/badge.svg?branch=master)](https://github.com/eladraz/reforge/actions/workflows/ci.yml)

Reforge
=======
Reforge translates a Windows-format RISC-V64 console program (PE32+, machine
`0x5064`) into a native x86-64 Linux ELF executable. It is both a small
binary-recompilation tool and the integration test of the semantic lifter,
legalizer and AMD64 lowering of [dismount](https://github.com/eladraz/dismount).

```
reforge --input hello-riscv64-windows.exe --target-arch x86_64 --target-os linux \
        --output-format elf --output hello-x86_64-linux
./hello-x86_64-linux            # prints exactly "hello-world\n", exit status 0
```

The output runs natively. No emulator, no interpreter and no source of the
guest program is involved: the translator reads only the `.exe`.

Architecture
============
```
PE32+ RV64   pe lib (+ PeLoader)         GuestImage: base, sections, entry, imports, IAT
    |
FlowRecovery dismount RISC-V decoder + SemanticLifter (rv64i only)
    |        basic blocks from the entry point; import calls found by local
    |        constant propagation of the IR; unresolved flow is rejected
    |
Amd64Lowerer dismount SemanticLegalizer (RV64 -> AMD64) + AMD64Lowering::lowerBlock
    |        (IA-32/AMD64 assembler of dismount encodes the bytes)
    |
BlockLinker  rel32 label resolution, entry/exit stubs, guest VA -> host offset table
MetaBuilder  guest image (IAT slots -> thunk VAs), block table, import table
ObjectEmitter ELF64 ET_REL object written by the elf library (cElf64ObjectWriter);
    |        fallback --object-writer asm: .S + .incbin assembled by the host `as`
HostLinker   host c++ links the object with libreforge_rt.a and the xStl archive
    v
x86-64 Linux ELF
```

Ownership (nothing of the instruction semantics lives in Reforge):

| Component | Does |
|---|---|
| pe (lib) | PE32+ parsing, `IMAGE_FILE_MACHINE_RISCV64`, import directory |
| dismount | RISC-V decode, lift to semantic IR, legalizer, AMD64 pattern lowering, AMD64 encoding, IR printer |
| elf (lib) | ELF64 relocatable object writer (sections, symbols) |
| xStl | portable file/stream API; all guest output goes through `cFile::getStandardStream(STANDARD_OUTPUT).writeAll` |
| host `c++`/`ld` | final link: crt, libc, libstdc++, the runtime and xStl. The elf library does not write executables (no program headers), so the executable is produced by the host linker |
| Reforge | orchestration, flow recovery policy, register map, block linking, metadata, runtime and import adapters |

### Translated code and runtime

* All 32 RISC-V registers and every IR temporary live in `reforge::GuestState`
  (`Include/reforge/GuestState.h`, offsets are `static_assert`ed). `r15` points
  to it, `r14` holds the bias of the guest memory (`host = guest VA + r14`),
  `rax/rcx/rdx` are scratch. Translated code never touches the host stack or
  calls the host.
* Guest memory is one non-executable heap region (image, import thunk page,
  1 MiB stack) at the preferred image base. Guest pointers stay guest VAs;
  adapters translate and bounds-check them (`GuestMemory`).
* Direct branches and calls are `jmp/jcc B_<guest va>`. Dynamic transfers
  (returns, calls through the IAT) store the target in `state.pc`, set
  `exitReason` (dismount `AMD64Lowering::ExitReason`) and leave through
  `__reforge_exit`; the runtime loop looks the target up in the block table or
  calls the import adapter selected by the thunk address.
* Import adapters are keyed by `module!symbol` (`Include/reforge/Meta.h`).
  Supported: `msvcrt.dll!printf` (literal text, `%s`, `%%`; any other
  conversion is a runtime error, exit 70) and `kernel32.dll!ExitProcess`.
* `REFORGE_TRACE=1` makes the runtime log the import dispatch and the xStl
  hand-off on stderr. A normal run prints nothing on stderr.

Requirements
============
Linux x86-64 host only. Needed to build and test:

* CMake >= 3.5 (3.20 or newer for `ctest --test-dir`)
* A C++14 compiler: clang 20 / gcc 11 tested
* Ninja or make
* git and python3 (stdlib only)
* LLVM tools `llvm-mc`, `ld.lld`, `llvm-readobj` and `llvm-objdump` (LLVM 20 tested)
* binutils (`readelf`, `objdump`, `nm`, `strings`, `as`) and `file`

Ubuntu (Debian)
---------------
`sudo apt-get install build-essential cmake git python3 llvm lld binutils file`
(add `clang` to build with clang)

No RISC-V toolchain is needed. The PE fixtures are assembled with `llvm-mc`
and linked with `ld.lld`, and `scripts/mkpe.py` (Python stdlib) writes the
PE32+ image.

The sibling checkouts `xStl`, `pe`, `elf` and `dismount` (branch `master`)
must live next to this repository.

Setting Up Environment
======================
Git
---
Reforge and its four libraries are separate repositories. Clone them side by
side in one directory:

```
git clone https://github.com/eladraz/reforge
git clone https://github.com/eladraz/xStl
git clone https://github.com/eladraz/pe
git clone https://github.com/eladraz/elf
git clone https://github.com/eladraz/dismount
```

The layout is:

```
<parent>/
    reforge/
    xStl/
    pe/
    elf/
    dismount/
```

How to Build
============
CMake
-----
CMake is a cross-platform build system for many platforms and systems.
You can read more about it [here](https://cmake.org/install/)

Reforge is built with CMake against the four sibling checkouts. Each sibling
must be built first; its CMake project writes the static archive to
`<sibling>/out/lib` (`xStlcm_static`, `pe_static`, `elf_static`,
`dismount_static`), and the reforge project links against those archives.

```
cd <parent>

# Build the libraries, xStl first (build type defaults to Debug)
cmake -S xStl -B xStl/build -DCMAKE_BUILD_TYPE=Release
cmake --build xStl/build -j

# pe, elf and dismount find xStl through the XSTL_PATH environment variable
export XSTL_PATH="$PWD/xStl"
cmake -S pe -B pe/build -DCMAKE_BUILD_TYPE=Release
cmake --build pe/build -j
cmake -S elf -B elf/build -DCMAKE_BUILD_TYPE=Release
cmake --build elf/build -j
cmake -S dismount -B dismount/build -DCMAKE_BUILD_TYPE=Release -DDISMOUNT_BUILD_TESTS=ON
cmake --build dismount/build -j

# Build reforge, the unit tests and the PE fixtures
cmake -S reforge -B reforge/build -DCMAKE_BUILD_TYPE=Release
cmake --build reforge/build -j
```

The output is written to `reforge/build/out`:

* `out/bin/reforge`: the translator
* `out/bin/test_reforge`: the unit tests
* `out/fixtures/`: the RISC-V64 PE fixtures (see below)

The sibling libraries are looked up at `../xStl`, `../pe`, `../elf` and
`../dismount`. Override them with `-DXSTL_PATH=`, `-DPELIB_PATH=`,
`-DELFLIB_PATH=`, `-DDISMOUNT_PATH=` or the environment variables of the same
names.

The `reforge_fixtures` target builds the fixtures with
`scripts/build_fixtures.sh`. It is defined only when `llvm-mc`, `ld.lld` and
`python3` are found.

Testing
=======
Unit tests
----------
```
ctest --test-dir reforge/build --output-on-failure
```

`ctest` runs `test_reforge`. The unit tests link the sibling libraries as they
are built in their `out/` directories, so rebuild xStl, pe, elf and dismount on
their current branches first.

End-to-end test
---------------
```
reforge/scripts/e2e.sh [BUILD_DIR]        # default: reforge/out/e2e, wiped on every run
```

`scripts/e2e.sh` builds everything from clean copies and runs the acceptance
checks. It does not change the working trees:

* it builds and tests xStl, pe, elf and dismount from `git archive HEAD` copies
  of the sibling checkouts (siblings are found next to reforge, or in
  `REFORGE_SIBLINGS`), then builds reforge and runs `test_reforge`;
* it builds the fixtures twice and compares their hashes;
* it checks the source identity of the input (llvm-readobj, dumpPE, and the
  dismount decoder against the `llvm-objdump` instruction stream);
* it translates with every dump, checks the target identity (`file`, `readelf`,
  `objdump`, and a byte compare of `--dump-target` against the executable);
* it runs the translated programs natively and compares stdout bytes, stderr
  and exit status;
* it checks trace evidence of the xStl path, data and semantic variation
  (including fixtures generated from a random message and a random loop
  count), the failure cases, and the anti-shortcut checks (no fixture message
  in the translator, the runtime or the generated code; no interpreter symbols
  in the output);
* it writes `versions.txt` with the toolchain versions and the git revisions of
  the five repositories.

It ends with `All N checks passed.` and exits 0 only if every check passed.

Fixtures
--------
The fixtures are Windows-format RISC-V64 console programs (RV64I only) under
`fixtures/`. Their contract (image format, sections, calling convention, import
calls through the IAT, the instruction subset) is in `fixtures/ABI.md`.
`scripts/build_fixtures.sh [OUTDIR]` writes them deterministically
(`hello`, `compute`, the `bad/` negative cases, and `SHA256SUMS`).

What is tested
--------------
| Area | State | Evidence |
|---|---|---|
| Source | PE32+ / RISC-V64 / console only; others rejected with exit 3 | `test_reforge`, e2e (wrong machine, truncated, missing file) |
| Guest ISA | RV64I, no compressed/M/A/F/D/V. dismount lowers all 230 RV64I rows (add..sd, branches, jal, jalr, shifts, w-forms, slt, lui, auipc, loads, stores, fence) identically to the reference results | dismount `TestSemanticLowering` (differential, 230/230). Through Reforge end to end only the instructions the fixtures use: `add addi andi auipc beq bne jal jalr ld lw sd` (and `li`, `j`) |
| Control flow | direct jal/branches, loops, calls to constants, returns, import calls through the IAT | hello, compute fixtures |
| Rejected statically | `ecall`/`ebreak` (exit 4), undecodable, RV64M and compressed words (exit 4), any indirect branch or call that is not a `ret` through ra or an IAT-slot import call (exit 6, including `jr t0`; an import call that does not link through `ra` and a branch or call target that is not 4-byte aligned are exit 6 too), flow leaving executable sections (exit 6), import without adapter (exit 5), malformed import tables (exit 3) | e2e and `test_reforge`: ecall, unresolved import, indirect call and jump, import call linking through t0, misaligned target, garbage/zero/mul/compressed words, six damaged import-table variants, stale output removal |
| Imports | `msvcrt.dll!printf` (literal, `%s`, `%%`), `kernel32.dll!ExitProcess` | e2e |
| Target | x86-64 Linux ELF (EXEC, dynamically linked against libstdc++/libc) | e2e (`file`, `readelf`, `objdump`) |
| Object writers | `elflib` (default) and `asm` | e2e runs hello with both |

Usage
=====
```
reforge/build/out/bin/reforge --input reforge/build/out/fixtures/hello-riscv64-windows.exe \
        --target-arch x86_64 --target-os linux --output-format elf --output hello-x86_64-linux
./hello-x86_64-linux            # prints exactly "hello-world\n", exit status 0
```

`reforge --help` lists all options. Two of them are worth knowing:

* `--object-writer elflib|asm`: the object writer (default `elflib`). `asm`
  writes assembler source with `.incbin` and assembles it with the host `as`.
* `--dump-dir <dir>`: write the dumps into a directory instead of stderr.

The dumps are `--dump-disasm` (RISC-V text per block), `--dump-ir` (lifted IR,
with guest addresses), `--dump-legalized` (IR after the legalizer),
`--dump-target` (the generated AMD64 re-disassembled by dismount from the
linked code, each run of instructions headed by the guest instruction it came
from), `--dump-imports` and `--dump-addrmap` (guest block to text offset).

Diagnostics go to stderr, never to stdout. A failed translation leaves no
output file.

Exit status
-----------
| Status | Meaning |
|---|---|
| 0 | OK |
| 1 | internal error |
| 2 | usage (bad command line) |
| 3 | malformed input (cannot be read, or not a well-formed PE32+ RISC-V image) |
| 4 | unsupported instruction (reachable, cannot be decoded or lifted) |
| 5 | unresolved import (no adapter) |
| 6 | unresolved control flow (cannot be resolved statically) |
| 7 | host link failure |

A translated program exits with the guest `ExitProcess` status (its low 8 bits),
or with `a0 & 0xff` when the entry function returns. It exits with 70 on a
runtime fault: a bad guest pointer, an unsupported `printf` conversion, an
unresolved guest pc, or a trap.

Limitations
===========
* Milestone 1 only: one source (PE32+ RV64 console) and one target
  (x86-64 Linux); the configuration types keep source and target separate so
  more can be added.
* Only RV64I PE32+ to x86-64 Linux ELF is supported (no other source or target
  architecture or format).
* RV64I only; `ecall` is not translated (no syscalls). No FPU, no atomics.
  Of the RV64I operations only those the lowering needs are lowered; any other
  IR operation makes the translator throw with the operation name and the guest
  address (the dismount AMD64 lowering does not cover vector, FP, atomic and
  similar operations).
* The output executable is dynamically linked against libstdc++ and libc (the
  runtime is C++ built on xStl); it is not static or freestanding.
* Every guest register and temporary is spilled to memory around each IR
  instruction and every dynamic control transfer goes through the runtime
  dispatcher: the output is correct but slow. No register caching.
* Translated loads and stores are not bounds-checked (`[r14 + rax]`): only
  unmapped host addresses are caught (runtime exit 70). A wild guest pointer
  can touch host memory if the computed host address is mapped. An unmapped
  address is caught by the runtime (SIGSEGV/SIGBUS handler: exit 70 and a
  message with the guest address; see `fixtures/bad/oob.S`). Pointers handed to
  imports are validated. Guest memory has no per-section permissions (the guest
  can overwrite its own `.text`/`.rdata` bytes; translated code is not
  affected). The output is non-PIE EXEC; translated code is in an R-X segment
  and no segment is both writable and executable. The guest stack is 16-byte
  aligned and `ra` starts as a sentinel whose `ret` ends the program with `a0`.
* Only statically visible code is translated: self-modifying code, jump tables
  and indirect branches are rejected, not emulated.
* `printf` supports only literal text, `%s` and `%%`; any other conversion
  (width/precision/length flags, numeric conversions) fails at runtime with
  exit 70. The whole output is formatted before any byte is written.
* The runtime fault handler uses POSIX `sigaction`/`write(2)` directly, only on
  the crash path (documented in `Runtime/Source/Runtime.cpp`).
* Not verified: Windows or macOS hosts, big-endian hosts, any x86-64 libc other
  than the one of the build machine. The Visual Studio project changes (the four
  libraries) and the Windows code (xStl `winFile.cpp` `getStandardStream` /
  `writeAll`) were never compiled.
* The autotools files (`configure.in`, `Makefile.am`) follow the CMake build
  (driver, `Amd64Lowerer.cpp`, runtime archive) but are not build-tested.
