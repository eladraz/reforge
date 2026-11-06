# bad/ecall.S - negative fixture: a reachable RV64 `ecall` (no syscalls allowed).
# Expected: reforge rejects the program with exit 4 naming the ecall address.
#
# @import kernel32.dll!ExitProcess

    .text
    .globl _start
    .type _start,@function
_start:
    li    a7, 93                    # Linux exit number: not a supported guest call
    li    a0, 0
    ecall                           # reachable ecall -> unsupported instruction

.Lexit:
    auipc t0, %pcrel_hi(__imp_ExitProcess)
    ld    t0, %pcrel_lo(.Lexit)(t0)
    jalr  ra, 0(t0)                 # ExitProcess(0), never reached by the translator

.Lhalt:
    jal   zero, .Lhalt
