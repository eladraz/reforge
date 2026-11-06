#!/usr/bin/env bash
# Build the Reforge RISC-V64 PE32+ fixtures deterministically.
#
#   scripts/build_fixtures.sh [OUTDIR]     (default: <repo>/out/fixtures)
#
# Produces, under OUTDIR:
#   hello-riscv64-windows.exe            printf("hello-world\n"); ExitProcess(0)
#   hello-alt-riscv64-windows.exe        same code, different message (data variation)
#   compute-riscv64-windows.exe          sums 1..5, expects 15 -> "sum-ok", exit 15
#   compute-alt-riscv64-windows.exe      sums 1..4, expects 10 -> "sum-ok", exit 10
#   compute7-riscv64-windows.exe         sums 1..7, expects 28 -> "sum-ok", exit 28
#   compute-mismatch-riscv64-windows.exe sums 1..5, expects 14 -> "sum-bad", exit 15
#   bad/ecall-riscv64-windows.exe        reachable ecall (must be rejected, exit 4)
#   bad/unresolved_import-riscv64-windows.exe  imports kernel32.dll!Sleep (exit 5)
#   bad/indirect-call-...exe, bad/indirect-jump-...exe  code pointer from .data (exit 6)
#   bad/garbage-...exe, bad/zeroword-...exe, bad/mul-...exe, bad/compressed-...exe
#                                        undecodable / RV64M / RVC word on the executed path (exit 4)
#   bad/linkreg-...exe                   import call linking through t0, not ra (exit 6)
#   bad/misaligned-...exe                jump to a target that is not 4-byte aligned (exit 6)
#   bad/oob-riscv64-windows.exe          load outside guest memory (translates; runtime exit 70)
#   SHA256SUMS                           checksums of every .exe above
#   work/<name>/                         intermediates (guest.o, idata.S, image.lds, image.elf)
#
# Tools (from PATH): llvm-mc, ld.lld, python3 (stdlib only). Output is
# byte-identical across runs and across output directories.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_ARG="${1:-$ROOT/out/fixtures}"
mkdir -p "$OUT_ARG"
OUT="$(cd "$OUT_ARG" && pwd)"
MKPE="$ROOT/scripts/mkpe.py"
FIX="$ROOT/fixtures"

for tool in llvm-mc ld.lld python3 sha256sum; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "build_fixtures.sh: error: required tool '$tool' not found in PATH" >&2
    exit 1
  fi
done

mkdir -p "$OUT/bad" "$OUT/work"

# build NAME SOURCE EXE-RELATIVE-PATH [mkpe options...]
build() {
  local name="$1" src="$2" exe="$3"
  shift 3
  python3 -I "$MKPE" "$@" "$src" -o "$OUT/$exe" --workdir "$OUT/work/$name"
}

build hello "$FIX/hello.S" hello-riscv64-windows.exe \
  --string 'msg=hello-world\n'

build hello-alt "$FIX/hello.S" hello-alt-riscv64-windows.exe \
  --string 'msg=goodbye-world\n'

build compute "$FIX/compute.S" compute-riscv64-windows.exe \
  --define SUM_N=5 --define SUM_EXPECT=15 \
  --string 'msg_ok=sum-ok\n' --string 'msg_bad=sum-bad\n'

build compute-alt "$FIX/compute.S" compute-alt-riscv64-windows.exe \
  --define SUM_N=4 --define SUM_EXPECT=10 \
  --string 'msg_ok=sum-ok\n' --string 'msg_bad=sum-bad\n'

build compute7 "$FIX/compute.S" compute7-riscv64-windows.exe \
  --define SUM_N=7 --define SUM_EXPECT=28 \
  --string 'msg_ok=sum-ok\n' --string 'msg_bad=sum-bad\n'

build compute-mismatch "$FIX/compute.S" compute-mismatch-riscv64-windows.exe \
  --define SUM_N=5 --define SUM_EXPECT=14 \
  --string 'msg_ok=sum-ok\n' --string 'msg_bad=sum-bad\n'

build bad-ecall "$FIX/bad/ecall.S" bad/ecall-riscv64-windows.exe

build bad-unresolved-import "$FIX/bad/unresolved_import.S" \
  bad/unresolved_import-riscv64-windows.exe

build bad-indirect-call "$FIX/bad/indirect.S" bad/indirect-call-riscv64-windows.exe \
  --define MODE=0
build bad-indirect-jump "$FIX/bad/indirect.S" bad/indirect-jump-riscv64-windows.exe \
  --define MODE=1
# raw words: the builder's RV64I-only word check is switched off for these
build bad-garbage "$FIX/bad/undecodable.S" bad/garbage-riscv64-windows.exe \
  --raw-words --define COMPRESSED=0 --define WORD=0xffffffff
build bad-zeroword "$FIX/bad/undecodable.S" bad/zeroword-riscv64-windows.exe \
  --raw-words --define COMPRESSED=0 --define WORD=0
build bad-mul "$FIX/bad/undecodable.S" bad/mul-riscv64-windows.exe \
  --raw-words --define COMPRESSED=0 --define WORD=0x02b50533
build bad-compressed "$FIX/bad/undecodable.S" bad/compressed-riscv64-windows.exe \
  --raw-words --define COMPRESSED=1 --define WORD=0x0001
build bad-linkreg "$FIX/bad/linkreg.S" bad/linkreg-riscv64-windows.exe
build bad-misaligned "$FIX/bad/misaligned.S" bad/misaligned-riscv64-windows.exe
build bad-oob "$FIX/bad/oob.S" bad/oob-riscv64-windows.exe

(
  cd "$OUT"
  sha256sum hello-riscv64-windows.exe hello-alt-riscv64-windows.exe \
    compute-riscv64-windows.exe compute-alt-riscv64-windows.exe \
    compute7-riscv64-windows.exe compute-mismatch-riscv64-windows.exe \
    bad/ecall-riscv64-windows.exe bad/unresolved_import-riscv64-windows.exe \
    bad/indirect-call-riscv64-windows.exe bad/indirect-jump-riscv64-windows.exe \
    bad/garbage-riscv64-windows.exe bad/zeroword-riscv64-windows.exe \
    bad/mul-riscv64-windows.exe bad/compressed-riscv64-windows.exe \
    bad/linkreg-riscv64-windows.exe \
    bad/misaligned-riscv64-windows.exe \
    bad/oob-riscv64-windows.exe > SHA256SUMS
)

echo "build_fixtures.sh: wrote fixtures to $OUT"
cat "$OUT/SHA256SUMS"
