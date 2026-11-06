# bad/linkreg.S - negative fixture: an import call that links through t0, not ra.
# The runtime resumes after an import call at ra, so the translator rejects the
# call instead of translating it wrongly. Expected: reforge exits 6 naming the
# guest address of the jalr.
#
# @import kernel32.dll!ExitProcess

    .text
    .globl _start
    .type _start,@function
_start:
    li    a0, 0
.Lexit:
    auipc t0, %pcrel_hi(__imp_ExitProcess)
    ld    t0, %pcrel_lo(.Lexit)(t0)
    jalr  t0, 0(t0)                     # links through t0 (x5): not supported

.Lhalt:
    jal   zero, .Lhalt
