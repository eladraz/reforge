# bad/oob.S - runtime-fault fixture: a load from a guest address outside the guest
# region (the translated load is [bias + 16]). This is NOT rejected at translation
# time (the address is computed); the runtime reports a fault: exit 70 and a
# message on stderr, nothing on stdout.
#
# @import kernel32.dll!ExitProcess

    .text
    .globl _start
    .type _start,@function
_start:
    li    t0, 16
    ld    a0, 0(t0)                 # guest address 0x10 is not mapped
.Lexit:
    auipc t0, %pcrel_hi(__imp_ExitProcess)
    ld    t0, %pcrel_lo(.Lexit)(t0)
    jalr  ra, 0(t0)

.Lhalt:
    jal   zero, .Lhalt
