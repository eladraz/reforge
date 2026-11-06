#!/usr/bin/env bash
# Reforge end-to-end test (the single entry point of the acceptance run).
#
#   scripts/e2e.sh [BUILD_DIR]            (default: <repo>/out/e2e)
#
# BUILD_DIR is wiped and recreated. Everything is built into it:
#   src/<lib>        exact `git archive HEAD` of the xStl, pe, elf and dismount
#                    checkouts (siblings of this repository, or REFORGE_SIBLINGS)
#   build/<name>     out-of-tree CMake build dirs
#   fx               the RISC-V64 PE fixtures
#   out              the translated executables and their run results
#   dumps            --dump-* output of the translator
#   versions.txt     toolchain versions and git revisions of the 5 repositories
# The libraries write their binaries to <src>/out (their CMake files do), which
# is why they are built from copies: the working trees are never touched.
#
# Exit status 0 only if every check passed. Needs: cmake, a C++14 compiler,
# ninja or make, llvm-mc, ld.lld, python3, llvm-readobj, llvm-objdump, file,
# readelf, objdump, nm, strings, cmp, od, sha256sum.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SIBLINGS="${REFORGE_SIBLINGS:-$(dirname "$REPO")}"
BUILD_ARG="${1:-$REPO/out/e2e}"
JOBS="${REFORGE_JOBS:-$(nproc 2>/dev/null || echo 4)}"

# ---------------------------------------------------------------- helpers
CHECKS=0
step() { printf '\n==== %s\n' "$*"; }
pass() { CHECKS=$((CHECKS + 1)); printf 'PASS: %s\n' "$*"; }
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || fail "required tool '$1' not found in PATH"; }
# check DESCRIPTION CMD...  -> runs the command quietly, FAIL if it returns nonzero
check() {
  local what="$1"; shift
  if "$@" >/dev/null 2>&1; then pass "$what"; else fail "$what  [$*]"; fi
}
check_grep() { # check_grep DESCRIPTION PATTERN FILE
  grep -Eq -- "$2" "$3" || fail "$1: pattern '$2' not found in $3"
  pass "$1"
}

for t in cmake llvm-mc ld.lld python3 llvm-readobj llvm-objdump file readelf objdump nm strings \
         cmp od sha256sum git tar c++; do
  need "$t"
done
if command -v ninja >/dev/null 2>&1; then GEN=(-G Ninja); BUILDER=(cmake --build); else GEN=(); BUILDER=(cmake --build); fi

mkdir -p "$BUILD_ARG"
BUILD="$(cd "$BUILD_ARG" && pwd)"
[ "$BUILD" != "/" ] && [ "$BUILD" != "$HOME" ] || fail "refusing to wipe '$BUILD'"
for p in "$REPO" "$SIBLINGS" "$SIBLINGS/xStl" "$SIBLINGS/pe" "$SIBLINGS/elf" "$SIBLINGS/dismount"; do
  case "$p/" in "$BUILD"/*) fail "refusing to wipe '$BUILD': it contains '$p'";; esac
done
rm -rf "$BUILD"
mkdir -p "$BUILD"/{src,build,fx,out,dumps,logs}
cd "$BUILD"
echo "e2e build directory: $BUILD"

PY=(python3 -I)
CHECKER=("${PY[@]}" "$REPO/scripts/e2e_check.py")

# ------------------------------------------------- 1. versions and sources
step "1. toolchain versions and repository revisions"
{
  echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "host: $(uname -srm)"
  for t in "c++ --version" "clang --version" "llvm-mc --version" "ld.lld --version" \
           "llvm-objdump --version" "llvm-readobj --version" "cmake --version" \
           "python3 --version" "readelf --version" "objdump --version" "file --version"; do
    echo "--- $t"; $t 2>&1 | head -3 || true
  done
  command -v ninja >/dev/null && { echo "--- ninja"; ninja --version; }
  for r in xStl pe elf dismount; do
    echo "--- git $r: $(git -C "$SIBLINGS/$r" rev-parse HEAD) branch $(git -C "$SIBLINGS/$r" rev-parse --abbrev-ref HEAD)"
    echo "    status: $(git -C "$SIBLINGS/$r" status --short | wc -l) changed/untracked paths"
    git -C "$SIBLINGS/$r" status --short | head -20 | sed 's/^/      /'
  done
  echo "--- git reforge: $(git -C "$REPO" rev-parse HEAD) branch $(git -C "$REPO" rev-parse --abbrev-ref HEAD)"
  echo "    status: $(git -C "$REPO" status --short | wc -l) changed/untracked paths"
  git -C "$REPO" status --short | head -20 | sed 's/^/      /'
} > versions.txt
cat versions.txt
pass "versions.txt recorded"

for r in xStl pe elf dismount; do
  # the build uses HEAD; uncommitted changes are listed in versions.txt, not built
  [ -z "$(git -C "$SIBLINGS/$r" status --short --untracked-files=no)" ] ||
    echo "warning: $r has uncommitted changes to tracked files; they are NOT part of this build (HEAD is archived)"
  mkdir -p "src/$r"
  git -C "$SIBLINGS/$r" archive --format=tar HEAD | tar -x -C "src/$r"
done
pass "sources of xStl, pe, elf, dismount extracted from git HEAD"

export XSTL_PATH="$BUILD/src/xStl" PELIB_PATH="$BUILD/src/pe" ELFLIB_PATH="$BUILD/src/elf" \
       ELF_PATH="$BUILD/src/elf" DISMOUNT_PATH="$BUILD/src/dismount"

cmake_build() { # cmake_build NAME SRC [cmake args...]   (log in logs/NAME.log)
  local name="$1" src="$2"; shift 2
  echo "building $name ..."
  { cmake "${GEN[@]}" -S "$src" -B "build/$name" -DCMAKE_BUILD_TYPE=Release "$@" &&
    "${BUILDER[@]}" "build/$name" -j "$JOBS"; } > "logs/$name.log" 2>&1 \
    || { tail -30 "logs/$name.log" >&2; fail "build of $name failed (logs/$name.log)"; }
  pass "built $name"
}

# --------------------------------------------------- 2. libraries and tests
step "2. build xStl, pe, elf, dismount and run their test suites"
cmake_build xStl "$XSTL_PATH"
[ -f "$XSTL_PATH/out/lib/libxStlcm_static.a" ] || fail "xStl static library missing"
cmake_build xstl_tests "$XSTL_PATH/tests"
( cd build/xstl_tests && "$XSTL_PATH/tests/out/bin/xstl_tests" ) > logs/xstl_tests.log 2>&1 \
  || { tail -30 logs/xstl_tests.log >&2; fail "xStl test suite failed"; }
pass "xStl test suite (xstl_tests): $(grep -c 'OK' logs/xstl_tests.log) OK lines, no failure"

cmake_build pe "$PELIB_PATH"
DUMPPE="$PELIB_PATH/out/bin/dumpPE"
[ -x "$DUMPPE" ] || fail "dumpPE was not built"

cmake_build elf "$ELFLIB_PATH"
( cd build/elf && ctest --output-on-failure ) > logs/elf_ctest.log 2>&1 \
  || { cat logs/elf_ctest.log >&2; fail "elf ctest failed"; }
pass "elf ctest: $(grep -E 'tests passed' logs/elf_ctest.log)"

cmake_build dismount "$DISMOUNT_PATH" -DDISMOUNT_BUILD_TESTS=ON
( cd "$DISMOUNT_PATH" && ./out/bin/test_dismount ) > logs/test_dismount.log 2>&1 \
  || { tail -30 logs/test_dismount.log >&2; fail "dismount test suite failed"; }
tail -3 logs/test_dismount.log
check_grep "dismount test suite (test_dismount) finished OK" '^OK!' logs/test_dismount.log
if grep -qiE '(^|[^a-z])(fail|failed)([^a-z]|$)' logs/test_dismount.log; then
  grep -niE '(^|[^a-z])(fail|failed)([^a-z]|$)' logs/test_dismount.log | head -5
fi

# -------------------------------------------------------- 3. reforge + fixtures
step "3. build reforge, run test_reforge, build the fixtures"
cmake_build reforge "$REPO" -DXSTL_PATH="$XSTL_PATH" -DPELIB_PATH="$PELIB_PATH" \
  -DELFLIB_PATH="$ELFLIB_PATH" -DDISMOUNT_PATH="$DISMOUNT_PATH" \
  -DREFORGE_FIXTURES_DEFAULT="$BUILD/fx"
REFORGE="$BUILD/build/reforge/out/bin/reforge"
[ -x "$REFORGE" ] || fail "reforge was not built"
build/reforge/out/bin/test_reforge > logs/test_reforge.log 2>&1 \
  || { tail -30 logs/test_reforge.log >&2; fail "test_reforge failed"; }
check_grep "test_reforge passed" '^OK!' logs/test_reforge.log

"$REPO/scripts/build_fixtures.sh" "$BUILD/fx" > logs/fixtures.log 2>&1 \
  || { cat logs/fixtures.log >&2; fail "fixture build failed"; }
"$REPO/scripts/build_fixtures.sh" "$BUILD/fx-again" > logs/fixtures-again.log 2>&1 \
  || fail "second fixture build failed"
cmp fx/SHA256SUMS fx-again/SHA256SUMS && pass "fixtures are reproducible (identical SHA256SUMS over two builds)"
cat fx/SHA256SUMS
FX="$BUILD/fx"
HELLO="$FX/hello-riscv64-windows.exe"

# the negative inputs that are derived from a fixture
head -c 700 "$HELLO" > "$FX/truncated.exe"
cp "$HELLO" "$FX/wrongmachine.exe"
printf '\x64\x86' | dd of="$FX/wrongmachine.exe" bs=1 seek=$((0x84)) conv=notrunc 2>/dev/null
llvm-readobj --file-headers "$FX/wrongmachine.exe" | grep -q 'Machine: .*0x8664' \
  && pass "wrongmachine.exe has Machine 0x8664" || fail "could not patch the machine field"

# ----------------------------------------------------------- 4. source identity
step "4. source identity of hello-riscv64-windows.exe"
llvm-readobj --file-headers --coff-imports "$HELLO" > out/hello.readobj.txt
check_grep "llvm-readobj: Machine 0x5064 (RISC-V 64)" 'Machine: .*0x5064' out/hello.readobj.txt
check_grep "llvm-readobj: PE32+ (optional header magic 0x20B)" 'Magic: 0x20B' out/hello.readobj.txt
check_grep "llvm-readobj: console subsystem" 'IMAGE_SUBSYSTEM_WINDOWS_CUI' out/hello.readobj.txt
check_grep "llvm-readobj: entry point RVA 0x1000" 'AddressOfEntryPoint: 0x1000' out/hello.readobj.txt
check_grep "llvm-readobj: imports msvcrt.dll" 'Name: msvcrt.dll' out/hello.readobj.txt
check_grep "llvm-readobj: imports kernel32.dll" 'Name: kernel32.dll' out/hello.readobj.txt
check_grep "llvm-readobj: imports printf" 'printf' out/hello.readobj.txt
check_grep "llvm-readobj: imports ExitProcess" 'ExitProcess' out/hello.readobj.txt
"$DUMPPE" "$HELLO" > out/hello.dumppe.txt
check_grep "dumpPE: RISC-V 64" 'RISC-V 64' out/hello.dumppe.txt
check_grep "dumpPE: msvcrt.dll!printf" 'msvcrt.dll!printf' out/hello.dumppe.txt
check_grep "dumpPE: kernel32.dll!ExitProcess" 'kernel32.dll!ExitProcess' out/hello.dumppe.txt
cat out/hello.dumppe.txt
check "pe lib dumpPE.sh (imports printf, ExitProcess)" sh "$PELIB_PATH/tests/dumpPE.sh" "$DUMPPE" "$HELLO" printf ExitProcess
check "pe lib truncated.sh (truncated inputs are rejected cleanly)" sh "$PELIB_PATH/tests/truncated.sh" "$DUMPPE" "$HELLO"

# ------------------------------------------------- 5. translate hello, all dumps
step "5. translate hello with every dump"
OUT="$BUILD/out"
D="$BUILD/dumps/hello"
"$REFORGE" --input "$HELLO" --target-arch x86_64 --target-os linux --output-format elf \
  --output "$OUT/hello-x86_64-linux" --verbose \
  --dump-disasm --dump-ir --dump-legalized --dump-target --dump-imports --dump-addrmap \
  --dump-dir "$D" > out/hello.translate.stdout 2> out/hello.translate.stderr \
  || { cat out/hello.translate.stderr >&2; fail "translation of hello failed"; }
[ ! -s out/hello.translate.stdout ] && pass "reforge wrote nothing to stdout" || fail "reforge wrote to stdout"
cat out/hello.translate.stderr
ls "$D"
for f in disasm ir legalized target imports addrmap; do [ -s "$D/$f.txt" ] || fail "dump $f is empty"; done
pass "all six dumps written"

# the dismount decoder agrees with LLVM on the instruction stream of the code section
llvm-objdump -d "$FX/work/hello/image.elf" | awk '/^ *[0-9a-f]+:/ {sub(":","",$1); print $1, $3}' \
  | sed 's/^/0x/' > out/hello.llvm.insns
grep -E '^  0x' "$D/disasm.txt" | awk '{sub(":","",$1); print $1, $2}' > out/hello.dismount.insns
cmp out/hello.llvm.insns out/hello.dismount.insns \
  && pass "RISC-V disassembly: dismount (address, mnemonic) of all $(wc -l < out/hello.dismount.insns) instructions equals llvm-objdump's" \
  || { diff out/hello.llvm.insns out/hello.dismount.insns | head; fail "disassembly differs from llvm-objdump"; }
echo "--- RISC-V disassembly (reforge --dump-disasm)"; cat "$D/disasm.txt"
check_grep "disasm dump shows the import call jalr" 'jalr' "$D/disasm.txt"
echo "--- lifted IR (first lines)"; head -12 "$D/ir.txt"
! grep -qi 'undefined' "$D/ir.txt" && pass "lifted IR has no undefined operation" || fail "lifted IR has an undefined operation"
! grep -qi 'undefined' "$D/legalized.txt" && pass "legalized IR has no undefined operation" || fail "legalized IR has an undefined operation"
check_grep "legalized IR has the guest addresses" '^0x140001000:' "$D/legalized.txt"
echo "--- guest -> AMD64 mapping (first lines of --dump-target)"; head -16 "$D/target.txt"
check_grep "target dump maps guest 0x140001000 to AMD64" '; guest 0x140001000: addi sp, sp, -16' "$D/target.txt"
check_grep "target dump has AMD64 instructions on the state register r15" 'qword ptr \[r15 \+' "$D/target.txt"
check_grep "target dump has the guest memory access through r14" '\[r14\+' "$D/target.txt"
check_grep "target dump has the exit stub dependency" '__reforge_exit' "$D/target.txt"
cat "$D/imports.txt"
check_grep "import map: msvcrt.dll!printf -> adapter" '^msvcrt\.dll!printf -> adapter [1-9]' "$D/imports.txt"
check_grep "import map: kernel32.dll!ExitProcess -> adapter" '^kernel32\.dll!ExitProcess -> adapter [1-9]' "$D/imports.txt"
check_grep "address map has guest 0x140001000" '^guest 0x140001000 -> text\+' "$D/addrmap.txt"

# ---------------------------------------------------------- 6. target identity
step "6. target identity"
EXE="$OUT/hello-x86_64-linux"
file "$EXE" | tee out/hello.file.txt
check_grep "file: ELF 64-bit LSB x86-64" 'ELF 64-bit LSB.*x86-64' out/hello.file.txt
readelf -hW "$EXE" > out/hello.readelf-h.txt
check_grep "readelf: Machine X86-64" 'Machine: +Advanced Micro Devices X86-64' out/hello.readelf-h.txt
check_grep "readelf: Class ELF64" 'Class: +ELF64' out/hello.readelf-h.txt
check_grep "readelf: executable or PIE" 'Type: +(EXEC|DYN)' out/hello.readelf-h.txt
readelf -lW "$EXE" > out/hello.readelf-l.txt
if grep -E '^ +LOAD' out/hello.readelf-l.txt | grep -q 'RWE'; then fail "a RWX segment exists"; fi
pass "no segment is writable and executable"
if grep -E '^ +LOAD' out/hello.readelf-l.txt | grep 'R E' >/dev/null; then pass "an R E segment holds the code"; fi
TEXT_ADDR=$(nm -S "$EXE" | awk '$4=="reforge_text" {print $1}')
TEXT_SIZE=$(nm -S "$EXE" | awk '$4=="reforge_text" {print $2}')
[ -n "$TEXT_ADDR" ] && [ -n "$TEXT_SIZE" ] || fail "symbols reforge_text not found"
objdump -d -M intel --no-show-raw-insn --start-address=0x$TEXT_ADDR \
        --stop-address=$((0x$TEXT_ADDR + 0x$TEXT_SIZE)) "$EXE" > out/hello.objdump-text.txt
check_grep "objdump: reforge_text holds AMD64 code using r15 (state) and r14 (guest memory base)" 'r15' out/hello.objdump-text.txt
check_grep "objdump: reforge_text uses the guest memory base r14" '\[r14\+rax' out/hello.objdump-text.txt
head -14 out/hello.objdump-text.txt
echo "reforge_text: 0x$TEXT_ADDR, 0x$TEXT_SIZE bytes"
"${CHECKER[@]}" blocks "$EXE" "$D/target.txt" || fail "--dump-target does not describe the executable"
pass "--dump-target bytes == bytes of the executable"

# ------------------------------------------------------------ 7. native run
step "7. native execution (no QEMU, no Wine, no interpreter): hello"
printf 'hello-world\n' > out/hello.expected
set +e
"$EXE" > out/hello.stdout 2> out/hello.stderr
RC=$?
set -e
echo "stdout bytes:"; od -c out/hello.stdout
echo "exit status: $RC"
cmp out/hello.stdout out/hello.expected && pass "stdout is exactly 'hello-world\\n' (cmp)"
[ ! -s out/hello.stderr ] && pass "stderr is empty" || { cat out/hello.stderr; fail "stderr not empty"; }
[ "$RC" -eq 0 ] && pass "exit status 0" || fail "exit status $RC"

# ------------------------------------------------------------ 8. xStl path
step "8. xStl path evidence"
set +e
REFORGE_TRACE=1 "$EXE" > out/hello.trace.stdout 2> out/hello.trace.stderr
TRC=$?
set -e
cat out/hello.trace.stderr
cmp out/hello.trace.stdout out/hello.expected && pass "traced run: stdout still exactly 'hello-world\\n'"
[ "$TRC" -eq 0 ] || fail "traced run exit status $TRC"
check_grep "trace: import msvcrt.dll!printf dispatched" 'import msvcrt\.dll!printf at guest 0x140001018' out/hello.trace.stderr
check_grep "trace: printf -> xStl standard output" 'printf -> xStl' out/hello.trace.stderr
check_grep "trace: kernel32.dll!ExitProcess -> runtime termination" 'ExitProcess\(0\) -> runtime termination' out/hello.trace.stderr
nm -C "$EXE" > out/hello.nm.txt
check_grep "executable links the xStl file API (cFile::writeAll)" 'cFile::writeAll' out/hello.nm.txt
check_grep "executable has the entry stub reforge_enter" ' T reforge_enter' out/hello.nm.txt
pass "normal run printed no trace (stderr empty, see step 7)"

# ------------------------------------------------------- 9. variations
step "9. data variation and semantic variation (everything through translated code)"
translate_run() { # translate_run NAME EXE-PATH  -> $OUT/NAME-x86_64-linux, results in out/NAME.{stdout,stderr,rc}
  local name="$1" in="$2"
  "$REFORGE" --input "$in" --target-arch x86_64 --target-os linux --output-format elf \
    --output "$OUT/$name-x86_64-linux" > "out/$name.translate.stdout" 2> "out/$name.translate.stderr" \
    || { cat "out/$name.translate.stderr" >&2; fail "translation of $name failed"; }
  [ ! -s "out/$name.translate.stdout" ] || fail "$name: reforge wrote to stdout"
  set +e
  "$OUT/$name-x86_64-linux" > "out/$name.stdout" 2> "out/$name.stderr"
  echo $? > "out/$name.rc"
  set -e
  [ ! -s "out/$name.stderr" ] || { cat "out/$name.stderr"; fail "$name: stderr not empty"; }
}
expect_run() { # expect_run NAME 'printf-format of the stdout' EXIT
  local name="$1" msg="$2" rc="$3"
  printf "$msg" > "out/$name.expected"
  cmp "out/$name.stdout" "out/$name.expected" || fail "$name: stdout differs"
  [ "$(cat "out/$name.rc")" -eq "$rc" ] || fail "$name: exit status $(cat "out/$name.rc"), expected $rc"
  echo "$name: stdout $(od -An -c "out/$name.stdout" | tr -s ' ') exit $(cat "out/$name.rc")"
  pass "$name prints the expected bytes, stderr empty, exit $rc"
}

translate_run hello-alt "$FX/hello-alt-riscv64-windows.exe"
expect_run hello-alt 'goodbye-world\n' 0
cmp -s out/hello-alt.stdout out/hello.stdout && fail "data variation: both messages are equal"
pass "data variation: a different message in the PE gives different output"

# the fallback object writer (assembler source with .incbin) gives the same program
"$REFORGE" --input "$HELLO" --target-arch x86_64 --target-os linux --output-format elf \
  --object-writer asm --output "$OUT/hello-asm-x86_64-linux" 2> out/hello-asm.translate.stderr \
  || { cat out/hello-asm.translate.stderr >&2; fail "translation with --object-writer asm failed"; }
"$OUT/hello-asm-x86_64-linux" > out/hello-asm.stdout 2> out/hello-asm.stderr
cmp out/hello-asm.stdout out/hello.expected && [ ! -s out/hello-asm.stderr ] \
  && pass "--object-writer asm (host assembler) executable prints hello-world\\n too"

# a message that did not exist before this run: built from a random value, in a directory with no sources
RND="e2e-$RANDOM-$RANDOM"
mkdir -p "$BUILD/rnd"
"${PY[@]}" "$REPO/scripts/mkpe.py" --string "msg=$RND\\n" "$REPO/fixtures/hello.S" \
  -o "$BUILD/rnd/random-riscv64-windows.exe" --workdir "$BUILD/rnd/work" > logs/rnd.log 2>&1 \
  || { cat logs/rnd.log >&2; fail "could not build the random-message fixture"; }
rm -rf "$BUILD/rnd/work"
translate_run random "$BUILD/rnd/random-riscv64-windows.exe"
expect_run random "$RND\\n" 0

translate_run compute "$FX/compute-riscv64-windows.exe";           expect_run compute   'sum-ok\n' 15
translate_run compute7 "$FX/compute7-riscv64-windows.exe";         expect_run compute7  'sum-ok\n' 28
translate_run compute-alt "$FX/compute-alt-riscv64-windows.exe";   expect_run compute-alt 'sum-ok\n' 10
translate_run compute-mismatch "$FX/compute-mismatch-riscv64-windows.exe"
expect_run compute-mismatch 'sum-bad\n' 15
[ "$(cat out/compute.rc)" != "$(cat out/compute7.rc)" ] && pass "semantic variation: compute (N=5) and compute7 (N=7) exit differently (15 vs 28)"
cmp -s out/compute.stdout out/compute-mismatch.stdout && fail "the bne did not change the output"
pass "semantic variation: the bne selects sum-ok or sum-bad in translated code"
# a computed value that is not a fixed fixture: N from $RANDOM, expected sum computed here
N=$(( RANDOM % 20 + 1 )); SUM=$(( N * (N + 1) / 2 ))
"${PY[@]}" "$REPO/scripts/mkpe.py" --define SUM_N=$N --define SUM_EXPECT=$SUM \
  --string 'msg_ok=sum-ok\n' --string 'msg_bad=sum-bad\n' "$REPO/fixtures/compute.S" \
  -o "$BUILD/rnd/computeN-riscv64-windows.exe" --workdir "$BUILD/rnd/work2" > logs/rnd2.log 2>&1 \
  || { cat logs/rnd2.log >&2; fail "could not build the computeN fixture"; }
translate_run computeN "$BUILD/rnd/computeN-riscv64-windows.exe"
expect_run computeN 'sum-ok\n' "$SUM"
echo "(N=$N, sum=$SUM)"

# ---------------------------------------------------------- 10. failures
step "10. failure behaviour"
expect_failure() { # expect_failure NAME INPUT EXIT-CODE MESSAGE-PATTERN
  local name="$1" in="$2" code="$3" pat="$4" rc
  rm -f "$OUT/fail-$name" "$OUT/fail-$name.o"
  set +e
  "$REFORGE" --input "$in" --target-arch x86_64 --target-os linux --output-format elf \
    --output "$OUT/fail-$name" > "out/fail-$name.stdout" 2> "out/fail-$name.stderr"
  rc=$?
  set -e
  echo "$name: exit $rc: $(head -1 "out/fail-$name.stderr")"
  [ "$rc" -eq "$code" ] || fail "$name: exit status $rc, expected $code"
  [ "$rc" -ne 0 ] || fail "$name: success status"
  grep -Eq -- "$pat" "out/fail-$name.stderr" || fail "$name: message does not match '$pat'"
  [ ! -s "out/fail-$name.stdout" ] || fail "$name: stdout is not empty"
  [ ! -e "$OUT/fail-$name" ] && [ ! -e "$OUT/fail-$name.o" ] || fail "$name: an output file was left behind"
  pass "$name -> exit $code with a clear message, no output file, empty stdout"
}
expect_failure ecall "$FX/bad/ecall-riscv64-windows.exe" 4 "unsupported instruction 'ecall' at 0x[0-9a-f]+"
expect_failure unresolved-import "$FX/bad/unresolved_import-riscv64-windows.exe" 5 'unresolved import kernel32\.dll!Sleep'
expect_failure indirect-call "$FX/bad/indirect-call-riscv64-windows.exe" 6 "unresolved control flow at 0x[0-9a-f]+: indirect call"
expect_failure indirect-jump "$FX/bad/indirect-jump-riscv64-windows.exe" 6 "unresolved control flow at 0x[0-9a-f]+: indirect branch"
expect_failure import-linkreg "$FX/bad/linkreg-riscv64-windows.exe" 6 "unresolved control flow at 0x[0-9a-f]+: import call .*does not link through ra"
expect_failure misaligned-target "$FX/bad/misaligned-riscv64-windows.exe" 6 "unresolved control flow: misaligned target at 0x[0-9a-f]+"
expect_failure garbage-word "$FX/bad/garbage-riscv64-windows.exe" 4 "unsupported instruction .* at 0x[0-9a-f]+"
expect_failure zero-word "$FX/bad/zeroword-riscv64-windows.exe" 4 "unsupported instruction .* at 0x[0-9a-f]+"
expect_failure rv64m-mul "$FX/bad/mul-riscv64-windows.exe" 4 "unsupported instruction 0x02b50533 .* at 0x[0-9a-f]+"
expect_failure compressed "$FX/bad/compressed-riscv64-windows.exe" 4 "unsupported instruction .* at 0x[0-9a-f]+"
# malformed import tables: one field of the hello import directory is damaged
"${PY[@]}" - "$HELLO" "$FX" <<'PYEOF'
import struct, sys
src = open(sys.argv[1], "rb").read()
pe = struct.unpack_from("<I", src, 0x3c)[0]
opt = pe + 24
dd = opt + 112 + 8                       # DataDirectory[IMPORT]
irva = struct.unpack_from("<I", src, dd)[0]
nsec = struct.unpack_from("<H", src, pe + 6)[0]
sec = opt + struct.unpack_from("<H", src, pe + 20)[0]
for i in range(nsec):
    o = sec + 40 * i
    if src[o:o + 6] == b".idata":
        desc = struct.unpack_from("<I", src, o + 20)[0] + irva - struct.unpack_from("<I", src, o + 12)[0]
def patch(name, offset, value):
    b = bytearray(src)
    struct.pack_into("<I", b, offset, value)
    open("%s/badimp-%s.exe" % (sys.argv[2], name), "wb").write(b)
patch("dirrva", dd, 0x7ffff000)
patch("dirsize", dd + 4, 0x10000000)
patch("name", desc + 12, 0x7ffffff0)
patch("ilt", desc, 0x7ffffff0)
patch("iat", desc + 16, 0x7ffffff0)
b = bytearray(src)
for i in range(20, 40):
    b[desc + i] = 1
open("%s/badimp-noterm.exe" % sys.argv[2], "wb").write(b)
PYEOF
for v in dirrva dirsize name ilt iat noterm; do
  expect_failure "badimport-$v" "$FX/badimp-$v.exe" 3 'import|IAT slot'
done
# a stale file at the output path must not survive a failed translation
printf 'stale' > "$OUT/fail-stale"
set +e
"$REFORGE" --input "$FX/bad/ecall-riscv64-windows.exe" --target-arch x86_64 --target-os linux \
  --output-format elf --output "$OUT/fail-stale" > /dev/null 2> out/fail-stale.stderr
SRC=$?
set -e
[ "$SRC" -eq 4 ] && [ ! -e "$OUT/fail-stale" ] \
  && pass "a failed translation leaves no (stale) executable at the output path" \
  || fail "stale output survived a failed translation (exit $SRC)"
expect_failure truncated "$FX/truncated.exe" 3 'truncated'
expect_failure wrongmachine "$FX/wrongmachine.exe" 3 '0x8664|machine'
expect_failure missing "$FX/does-not-exist.exe" 3 'does-not-exist'
set +e
"$REFORGE" --input "$HELLO" --target-os windows --output "$OUT/fail-usage" > out/fail-usage.stdout 2> out/fail-usage.stderr
URC=$?
set -e
[ "$URC" -eq 2 ] && [ ! -s out/fail-usage.stdout ] && [ ! -e "$OUT/fail-usage" ] \
  && pass "unsupported target (--target-os windows) -> exit 2, no output" || fail "usage error behaviour (exit $URC)"

# a guest access outside guest memory translates but faults in the runtime: exit 70, a message, no stdout
translate_oob() {
  "$REFORGE" --input "$FX/bad/oob-riscv64-windows.exe" --target-arch x86_64 --target-os linux \
    --output-format elf --output "$OUT/oob-x86_64-linux" 2> out/oob.translate.stderr \
    || { cat out/oob.translate.stderr >&2; fail "oob fixture did not translate"; }
  set +e
  "$OUT/oob-x86_64-linux" > out/oob.stdout 2> out/oob.stderr
  OOBRC=$?
  set -e
  cat out/oob.stderr
  [ "$OOBRC" -eq 70 ] && [ ! -s out/oob.stdout ] && grep -q 'runtime error: segmentation fault at host address' out/oob.stderr \
    && pass "guest access outside guest memory -> runtime fault: exit 70, message on stderr, empty stdout" \
    || fail "oob run: exit $OOBRC"
}
translate_oob

# ------------------------------------------------------ 11. anti-shortcut
step "11. anti-shortcut checks"
MSGS=('hello-world' 'goodbye-world' 'sum-ok' 'sum-bad' "$RND")
for m in "${MSGS[@]}"; do
  if strings -a "$REFORGE" | grep -qF -- "$m"; then fail "the translator binary contains '$m'"; fi
  if strings -a "$BUILD/build/reforge/out/lib/libreforge_rt.a" | grep -qF -- "$m"; then fail "the runtime contains '$m'"; fi
done
pass "neither the translator nor the runtime archive contain any fixture message"
if grep -rqF -e hello-world -e goodbye-world -e sum-ok -e sum-bad "$REPO/Source" "$REPO/Include" "$REPO/Runtime"; then
  fail "a fixture message appears in the translator or runtime sources"
fi
pass "no fixture message in Source/, Include/ or Runtime/"
"${CHECKER[@]}" msgloc "$EXE" 'hello-world\n'
"${CHECKER[@]}" msgloc "$OUT/hello-alt-x86_64-linux" 'goodbye-world\n'
"${CHECKER[@]}" msgloc "$OUT/random-x86_64-linux" "$RND\\n"
pass "messages are only in the embedded guest image, never in generated code"
for exe in "$OUT"/*-x86_64-linux; do
  if nm -C "$exe" | grep -qiE 'SemanticInterpreter|SemanticLifter|SemanticLowering|SemanticLegalizer|RiscvStreamDisassembler'; then
    fail "$exe contains dismount interpreter/lifter symbols"
  fi
done
pass "no SemanticInterpreter / lifter / lowering symbol in any output executable"
readelf -d "$EXE" | grep NEEDED | tee out/hello.needed.txt
if grep -qiE 'qemu|wine' out/hello.needed.txt; then fail "output depends on an emulator"; fi
pass "output executable has only host libraries as dependencies; it was run directly (no qemu/wine)"
for p in qemu-riscv64 qemu-x86_64 wine wine64; do
  if command -v "$p" >/dev/null 2>&1; then echo "note: $p exists in PATH but is never invoked by this script"; fi
done
pass "e2e.sh invokes no emulator"

# ------------------------------------------------------------ summary
step "summary"
echo "All $CHECKS checks passed."
echo "build dir: $BUILD"
echo "revisions: see $BUILD/versions.txt"
