# bad/indirect.S - negative fixture: control flow that cannot be resolved statically.
# The jump/call target is a code address loaded from .data (NOT an IAT slot), so
# the translator cannot know it. Expected: reforge exits 6 naming the guest address.
# MODE (from defs.inc): 0 = indirect call (jalr ra, 0(t0)), 1 = indirect jump (jalr zero, 0(t0)).
#
# @import kernel32.dll!ExitProcess

    .include "defs.inc"

    .section .data,"aw",@progbits
    .balign 8
fnptr:
    .quad target

    .text
    .globl _start
    .type _start,@function
_start:
.Lptr:
    auipc t0, %pcrel_hi(fnptr)
    ld    t0, %pcrel_lo(.Lptr)(t0)      # t0 = a code pointer read from data
.if MODE == 0
    jalr  ra, 0(t0)                     # indirect call, target unknown
.else
    jalr  zero, 0(t0)                   # indirect jump, target unknown
.endif

target:
    li    a0, 0
.Lexit:
    auipc t0, %pcrel_hi(__imp_ExitProcess)
    ld    t0, %pcrel_lo(.Lexit)(t0)
    jalr  ra, 0(t0)

.Lhalt:
    jal   zero, .Lhalt
