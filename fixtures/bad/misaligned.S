# bad/misaligned.S - negative fixture: a direct jump to a target that is not
# 4-byte aligned (RV64I without C has IALIGN=32). Expected: reforge exits 6
# naming the target address.
#
# @import kernel32.dll!ExitProcess

    .text
    .globl _start
    .type _start,@function
_start:
    jal   zero, _start + 10             # 2 mod 4: misaligned target
    li    a0, 0
.Lexit:
    auipc t0, %pcrel_hi(__imp_ExitProcess)
    ld    t0, %pcrel_lo(.Lexit)(t0)
    jalr  ra, 0(t0)

.Lhalt:
    jal   zero, .Lhalt
