# bad/undecodable.S - negative fixture: an instruction word on the executed path
# that RV64I (no C, no M) does not define. Expected: reforge exits 4 naming the
# guest address. WORD is a 32-bit encoding or, for COMPRESSED=1, a 16-bit one
# (from defs.inc) emitted raw (the assembler is run with -c disabled).
#   garbage     0xffffffff  (not a valid encoding; low bits 11 11 select the 32-bit format)
#   zero word   0x00000000  (all-zero parcel is defined illegal)
#   mul         0x02b50533  (RV64M: not RV64I)
#   c.nop       0x0001      (RVC: compressed)
#
# @import kernel32.dll!ExitProcess

    .include "defs.inc"

    .text
    .globl _start
    .type _start,@function
_start:
    li    a0, 0
.if COMPRESSED == 1
    .2byte WORD
    .2byte 0x0001
.else
    .4byte WORD
.endif
.Lexit:
    auipc t0, %pcrel_hi(__imp_ExitProcess)
    ld    t0, %pcrel_lo(.Lexit)(t0)
    jalr  ra, 0(t0)

.Lhalt:
    jal   zero, .Lhalt
