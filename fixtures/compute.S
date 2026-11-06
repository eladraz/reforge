# compute.S - RISC-V64 (RV64I only) guest: data- and branch-dependent output.
#
# Sums 1..N (N is the .data word `count`), compares the sum with the .data word
# `expect` using bne, prints "sum-ok" or "sum-bad" through msvcrt.dll!printf,
# then calls kernel32.dll!ExitProcess(sum & 0xff).
# Defaults (set by scripts/build_fixtures.sh): N=5, expect=15 -> "sum-ok", exit 15.
#
# @import msvcrt.dll!printf
# @import kernel32.dll!ExitProcess

    .include "defs.inc"             # SUM_N, SUM_EXPECT

    .section .rdata,"a",@progbits
    .include "strings.inc"          # defines msg_ok and msg_bad

    .section .data,"aw",@progbits
    .balign 4
count:
    .word SUM_N                    # from defs.inc (.set SUM_N, <int>)
expect:
    .word SUM_EXPECT

    .text
    .globl _start
    .type _start,@function
_start:
    addi sp, sp, -16
    sd   ra, 0(sp)
    sd   s0, 8(sp)

    # s0 = 1 + 2 + ... + N, counting t1 down from N to 0.
.Lcount:
    auipc t0, %pcrel_hi(count)
    lw    t1, %pcrel_lo(.Lcount)(t0)     # t1 = N
    li    s0, 0
.Lloop:
    beq   t1, zero, .Lsum_done
    add   s0, s0, t1
    addi  t1, t1, -1
    jal   zero, .Lloop
.Lsum_done:

    # Branch on the result: bne to the "bad" message when sum != expect.
.Lexpect:
    auipc t0, %pcrel_hi(expect)
    lw    t2, %pcrel_lo(.Lexpect)(t0)    # t2 = expected sum
    bne   s0, t2, .Lbad

.Lmsg_ok:
    auipc a0, %pcrel_hi(msg_ok)
    addi  a0, a0, %pcrel_lo(.Lmsg_ok)
    jal   zero, .Lprint

.Lbad:
.Lmsg_bad:
    auipc a0, %pcrel_hi(msg_bad)
    addi  a0, a0, %pcrel_lo(.Lmsg_bad)

.Lprint:
.Lprintf:
    auipc t0, %pcrel_hi(__imp_printf)
    ld    t0, %pcrel_lo(.Lprintf)(t0)
    jalr  ra, 0(t0)                      # printf(msg)

    andi  a0, s0, 0xff                   # exit status = sum & 0xff
    ld    ra, 0(sp)
    ld    s0, 8(sp)
    addi  sp, sp, 16

.Lexit:
    auipc t0, %pcrel_hi(__imp_ExitProcess)
    ld    t0, %pcrel_lo(.Lexit)(t0)
    jalr  ra, 0(t0)                      # ExitProcess(sum & 0xff)

.Lhalt:
    jal   zero, .Lhalt
