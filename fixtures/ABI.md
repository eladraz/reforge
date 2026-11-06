# Fixture guest ABI and import contract

These fixtures are the inputs for the Reforge milestone 1 pipeline. They are
Windows-format console programs for RISC-V64 (RV64I only). The translator
must consume only the PE file, never these sources.

## Image format

| Field | Value |
|---|---|
| Signature | `MZ` stub, `PE\0\0` at `e_lfanew = 0x80` |
| Machine | `0x5064` (`IMAGE_FILE_MACHINE_RISCV64`) |
| Optional header | PE32+, Magic `0x20B`, 16 data directories |
| Subsystem | 3 (`IMAGE_SUBSYSTEM_WINDOWS_CUI`, console) |
| ImageBase | `0x140000000` (no base relocations; `RELOCS_STRIPPED`) |
| Section alignment / file alignment | `0x1000` / `0x200` |
| TimeDateStamp | 0 (deterministic output) |
| Entry point | `_start`, RVA `0x1000` |

Sections (fixed RVAs; a section is omitted when it is empty):

| Section | RVA | Characteristics | Contents |
|---|---|---|---|
| `.text` | `0x1000` | code, read, execute | RV64I code (4-byte encodings only) |
| `.rdata` | `0x2000` | initialized data, read | messages (`.asciz`, NUL terminated) |
| `.data` | `0x3000` | initialized data, read, write | `.word` parameters |
| `.idata` | `0x4000` | initialized data, read, write | import directory, ILT, IAT, hint/name, DLL names |

Data directories used: index 1 (import directory) at `.idata+0` and index 12
(IAT) covering all IAT slots, which are contiguous.

## Guest calling convention

- RISC-V LP64 psABI: `a0`-`a7` arguments, `a0` return value, `ra` return
  address, `sp` 16-byte aligned, `s0`-`s11` callee-saved.
- Guest code calls an import through its IAT slot, as a PE `call [__imp_X]`:

  ```
  .Lx:  auipc t0, %pcrel_hi(__imp_printf)
        ld    t0, %pcrel_lo(.Lx)(t0)     # t0 = IAT slot (loader-patched)
        jalr  ra, 0(t0)
  ```

  The call must link through `ra` (`jalr ra`): the runtime resumes after an
  import call at `ra`, so an import call with any other link register (for
  example `jalr t0, 0(t0)`) is rejected with exit 6. A direct or indirect
  branch or call target must be 4-byte aligned (RV64I without C), otherwise
  the translator exits 6.

  The IAT slot contents before loading are the hint/name RVA, as in a PE file.
  The runtime replaces each slot with its adapter entry.
- Guest data addresses are PC-relative (`auipc` plus a low 12-bit offset). The
  image is linked at its ImageBase, but the guest must not depend on the
  absolute address beyond what the loader provides.
- Guest code never returns from `_start`. Each fixture ends with a call to
  `ExitProcess`, followed by a `j .` fallthrough that is never reached.

## Import contract

Each `@import MODULE!SYMBOL` line in a source declares one import. Each
referenced `__imp_SYMBOL` must have a declaration, and each declaration must
be referenced. `scripts/mkpe.py` enforces both. Imports are listed in the
order they appear in the source.

| Import | Used by | Expected translator behaviour |
|---|---|---|
| `msvcrt.dll!printf` | `hello.S`, `compute.S` | adapter; supported subset (literal text, `%s`, `%%`) |
| `kernel32.dll!ExitProcess` | `hello.S`, `compute.S`, `bad/ecall.S` | adapter; terminates the process with the status argument |
| `kernel32.dll!Sleep` | `bad/unresolved_import.S` | no adapter; exit 5 naming `kernel32.dll!Sleep` |

Argument and return conventions for the adapters:

- `printf(const char *fmt)`: `a0` is a guest pointer to a NUL-terminated
  string. The fixtures pass the message as the format string, so it must not
  contain `%`.
- `ExitProcess(UINT uExitCode)`: `a0` is the exit status (the low 8 bits are
  observed by the host).
- `Sleep(DWORD dwMilliseconds)`: `a0` is the duration (only used by the
  negative fixture).

## Fixtures

| File | Purpose | Expected behaviour |
|---|---|---|
| `hello.S` | data variation (message via `--string msg=`) | prints the message, exit 0 |
| `compute.S` | semantic variation: sums `1..N` (`.data` word `count`), `bne` to `msg_ok` or `msg_bad` when the sum differs from `expect` | prints `sum-ok\n` and exits with `sum & 0xff` when equal, `sum-bad\n` otherwise |
| `bad/ecall.S` | reachable `ecall` (no syscalls) | rejected at translation, exit 4 naming the address |
| `bad/unresolved_import.S` | import without an adapter | rejected at translation, exit 5 naming the import |

The message strings and numeric parameters come from the builder, not from the
sources. `scripts/build_fixtures.sh` supplies the defaults:

| Output | Command-line parameters |
|---|---|
| `hello-riscv64-windows.exe` | `msg='hello-world\n'` |
| `hello-alt-riscv64-windows.exe` | `msg='goodbye-world\n'` |
| `compute-riscv64-windows.exe` | `SUM_N=5 SUM_EXPECT=15 msg_ok='sum-ok\n' msg_bad='sum-bad\n'` (prints `sum-ok`, exit 15) |
| `compute-alt-riscv64-windows.exe` | `SUM_N=4 SUM_EXPECT=10` (prints `sum-ok`, exit 10) |
| `compute7-riscv64-windows.exe` | `SUM_N=7 SUM_EXPECT=28` (prints `sum-ok`, exit 28) |
| `compute-mismatch-riscv64-windows.exe` | `SUM_N=5 SUM_EXPECT=14` (the `bne` is taken: prints `sum-bad`, exit 15) |

The sources therefore assemble only through `scripts/mkpe.py`, which generates
`defs.inc` and `strings.inc` next to them. They are not standalone assembly files.

## Instruction subset

Only RV64I, with 32-bit encodings. No C (compressed), V, or F/D extensions.
`ecall` is used only by the negative fixture. The fixtures use `add`, `addi`,
`andi`, `auipc`, `beq`, `bne`, `jal`, `jalr`, `ld`, `lw`, `sd`, and `li`
(which assembles to `addi`).

## Build

```
scripts/build_fixtures.sh [OUTDIR]        # default: <repo>/out/fixtures
```

Required tools in `PATH`: `llvm-mc`, `ld.lld` (LLVM 20 tested), `python3`
(stdlib only), `sha256sum`. `mkpe.py` runs `llvm-mc -triple=riscv64
-mattr=-c,-relax`, then `ld.lld -m elf64lriscv --no-relax` with the generated
`image.lds`. It then reads the linked ELF64 and writes the PE32+ image itself.
The intermediates are kept in `OUTDIR/work/<name>/` (`guest.o`, `idata.S`,
`image.lds`, `image.elf`).
