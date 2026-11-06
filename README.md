# Reforge

Reforge translates a Windows-format RISC-V64 console program (PE32+, machine
`0x5064`) into a native x86-64 Linux ELF executable. It is both a small
binary-recompilation tool and the integration test of the semantic lifter,
legalizer and AMD64 lowering of [dismount](../dismount).

```
reforge --input hello-riscv64-windows.exe --target-arch x86_64 --target-os linux \
        --output-format elf --output hello-x86_64-linux
./hello-x86_64-linux            # prints exactly "hello-world\n", exit status 0
```

The output runs natively. No emulator, no interpreter and no source of the
guest program is involved: the translator reads only the `.exe`.

## Architecture

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
  `REFORGE_TRACE=1` makes the runtime log the import dispatch and the xStl
  hand-off on stderr; a normal run prints nothing on stderr.

### Dumps (to stderr, or files with `--dump-dir`)

`--dump-disasm` RISC-V text per block, `--dump-ir` lifted IR (with guest
addresses), `--dump-legalized` IR after the legalizer, `--dump-target` the
generated AMD64 re-disassembled by dismount from the linked code, each run of
instructions headed by the guest instruction it came from (the guest -> AMD64
mapping), `--dump-imports`, `--dump-addrmap` (guest block -> text offset).

## Prerequisites

Linux x86-64 host, CMake >= 3.5, a C++14 compiler (clang 20 / gcc 11 tested),
ninja or make, git, python3 (stdlib only), LLVM tools `llvm-mc`, `ld.lld`,
`llvm-readobj`, `llvm-objdump` (LLVM 20 tested), binutils (`readelf`,
`objdump`, `nm`), `file`. The sibling checkouts `xStl`, `pe`, `elf` and
`dismount` (branch `reforge-m1`) live next to this repository.

## Build and run

Everything, including all dependency builds, the test suites, fixtures and
the acceptance checks, from one command:

```
scripts/e2e.sh [BUILD_DIR]        # default: out/e2e, wiped on every run
```

By hand (the libraries write their binaries to `<repo>/out`):

```
export XSTL_PATH=/path/to/xStl
cmake -G Ninja -S xStl -B /tmp/b/xStl -DCMAKE_BUILD_TYPE=Release && ninja -C /tmp/b/xStl    # likewise pe, elf
cmake -G Ninja -S dismount -B /tmp/b/dismount -DCMAKE_BUILD_TYPE=Release && ninja -C /tmp/b/dismount
cmake -G Ninja -S reforge -B /tmp/b/reforge && ninja -C /tmp/b/reforge
/tmp/b/reforge/out/bin/test_reforge          # or: (cd /tmp/b/reforge && ctest --output-on-failure)
scripts/build_fixtures.sh /tmp/b/fx
/tmp/b/reforge/out/bin/reforge --input /tmp/b/fx/hello-riscv64-windows.exe \
    --target-arch x86_64 --target-os linux --output-format elf --output hello-x86_64-linux
```

`ctest` runs `test_reforge` (the unit tests). The unit tests link the sibling
libraries as built in their `out/` directories, so a local run needs xStl, pe,
elf and dismount rebuilt on their current branches first (`scripts/e2e.sh`
always builds all of them from clean `git archive HEAD` copies).

`reforge --help` lists all options. Exit status: 0 ok, 2 usage, 3 malformed
input, 4 unsupported instruction, 5 unresolved import, 6 unresolved control
flow, 7 host link failure, 1 internal error. Diagnostics go to stderr, never
to stdout; a failed translation leaves no output file. A translated program
exits with the guest `ExitProcess` status (or `a0 & 0xff` when the entry
returns) and 70 on a runtime fault (bad guest pointer, unsupported `printf`
conversion, unresolved guest pc, trap).

`scripts/e2e.sh` performs: build and test xStl, pe, elf, dismount (each from a
`git archive HEAD` copy, so the working trees are untouched) and reforge;
build the fixtures twice and compare hashes; check the source identity
(llvm-readobj, dumpPE, dismount vs llvm-objdump instruction stream); translate
with all dumps; check the target identity (file, readelf, objdump, byte-compare
of `--dump-target` against the executable); run natively and compare stdout
bytes, stderr and exit status; trace evidence of the xStl path; data and
semantic variation (including fixtures generated with a random message and a
random loop count); the failure cases; anti-shortcut checks (no fixture
message in the translator, the runtime or the generated code, no
interpreter symbols in the output); `versions.txt` with toolchain versions and
the git revisions of the five repositories.

## Fixture ABI

See `fixtures/ABI.md`. In short: PE32+ (magic `0x20B`), machine `0x5064`,
console subsystem, ImageBase `0x140000000`, no base relocations, RV64I only
with 4-byte encodings, LP64 convention (`a0`-`a7`, `ra`, 16-byte aligned
`sp`). Imports are called as `auipc t0,%pcrel_hi(__imp_X); ld t0,%pcrel_lo(..)(t0);
jalr ra,0(t0)`. The entry is called like a function with `ra` = a host
return sentinel; guest code ends in `ExitProcess`. The fixtures are built by
`scripts/build_fixtures.sh` (`llvm-mc -mattr=-c,-relax`, `ld.lld --no-relax`,
then `scripts/mkpe.py` writes the PE32+) and are byte-reproducible. They
cannot run natively on Windows (there is no Windows on RISC-V target here).

## Support matrix (what is tested)

| Area | State | Evidence |
|---|---|---|
| Source | PE32+ / RISC-V64 / console only; others rejected with exit 3 | `test_reforge`, e2e (wrong machine, truncated, missing file) |
| Guest ISA | RV64I, no compressed/M/A/F/D/V. dismount lowers all 230 RV64I rows (add..sd, branches, jal, jalr, shifts, w-forms, slt, lui, auipc, loads, stores, fence) identically to the reference results | dismount `TestSemanticLowering` (differential, 230/230). Through Reforge end to end only the instructions the fixtures use: `add addi andi auipc beq bne jal jalr ld lw sd` (and `li`, `j`) |
| Control flow | direct jal/branches, loops, calls to constants, returns, import calls through the IAT | hello, compute fixtures |
| Rejected statically | `ecall`/`ebreak` (exit 4), undecodable, RV64M and compressed words (exit 4), any indirect branch or call that is not a `ret` through ra or an IAT-slot import call (exit 6, including `jr t0`; an import call that does not link through `ra` and a branch or call target that is not 4-byte aligned are exit 6 too), flow leaving executable sections (exit 6), import without adapter (exit 5), malformed import tables (exit 3) | e2e and `test_reforge`: ecall, unresolved import, indirect call and jump, import call linking through t0, misaligned target, garbage/zero/mul/compressed words, six damaged import-table variants, stale output removal |
| Imports | `msvcrt.dll!printf` (literal, `%s`, `%%`), `kernel32.dll!ExitProcess` | e2e |
| Target | x86-64 Linux ELF (EXEC, dynamically linked against libstdc++/libc) | e2e (`file`, `readelf`, `objdump`) |
| Object writers | `elflib` (default) and `asm` | e2e runs hello with both |

## Limitations

* Milestone 1 only: one source (PE32+ RV64 console) and one target
  (x86-64 Linux); the configuration types keep source and target separate so
  more can be added.
* RV64I only; `ecall` is not translated (no syscalls). No FPU, no atomics.
  Of the RV64I operations only those the lowering needs are lowered; any
  other IR operation makes the translator throw with the operation name and
  the guest address (the dismount AMD64 lowering does not cover vector, FP,
  atomic and similar operations).
* Only RV64I PE32+ to x86-64 Linux ELF is supported (no other source or
  target architecture or format).
* The output executable is dynamically linked against libstdc++ and libc
  (the runtime is C++ built on xStl); it is not static or freestanding.
* Every guest register and temporary is spilled to memory around each IR
  instruction and every dynamic control transfer goes through the runtime
  dispatcher: the output is correct but slow. No register caching.
* Translated loads and stores are not bounds-checked (`[r14 + rax]`): only
  unmapped host addresses are caught (runtime exit 70). A wild
  guest pointer can touch host memory if the computed host address is mapped.
  An unmapped address is caught by the runtime (SIGSEGV/SIGBUS handler: exit 70
  and a message with the guest address; see `fixtures/bad/oob.S`). Pointers
  handed to imports are validated. Guest memory has no per-section permissions
  (the guest can overwrite its own `.text`/`.rdata` bytes; translated code is not
  affected). The output is non-PIE EXEC; translated code is in an R-X segment and
  no segment is both writable and executable. The guest stack is 16-byte aligned
  and `ra` starts as a sentinel whose `ret` ends the program with `a0`.
* Only statically visible code is translated: self-modifying code, jump
  tables and indirect branches are rejected, not emulated.
* Not verified: Windows or macOS hosts, big-endian hosts, any x86-64 libc
  other than the one of the build machine. The Visual Studio project changes
  (the four libraries) and the Windows code (xStl `winFile.cpp`
  `getStandardStream`/`writeAll`) were never compiled.
* The autotools files (`configure.in`, `Makefile.am`) follow the CMake build
  (driver, `Amd64Lowerer.cpp`, runtime archive) but are not build-tested.
* `printf` supports only literal text, `%s` and `%%`; any other conversion
  (width/precision/length flags, numeric conversions) fails at runtime with
  exit 70. The whole output is formatted before any byte is written.
* The runtime fault handler uses POSIX `sigaction`/`write(2)` directly, only
  on the crash path (documented in `Runtime/Source/Runtime.cpp`).
