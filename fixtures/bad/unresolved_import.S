# bad/unresolved_import.S - negative fixture: imports kernel32.dll!Sleep, which
# has no adapter in the Reforge runtime. Expected: exit 5 naming kernel32.dll!Sleep.
#
# @import kernel32.dll!Sleep
# @import kernel32.dll!ExitProcess

    .text
    .globl _start
    .type _start,@function
_start:
    addi  sp, sp, -16
    sd    ra, 0(sp)
    li    a0, 0                     # Sleep(0)
.Lsleep:
    auipc t0, %pcrel_hi(__imp_Sleep)
    ld    t0, %pcrel_lo(.Lsleep)(t0)
    jalr  ra, 0(t0)
    ld    ra, 0(sp)
    addi  sp, sp, 16
    li    a0, 0                     # ExitProcess(0)
.Lexit:
    auipc t0, %pcrel_hi(__imp_ExitProcess)
    ld    t0, %pcrel_lo(.Lexit)(t0)
    jalr  ra, 0(t0)

.Lhalt:
    jal   zero, .Lhalt
