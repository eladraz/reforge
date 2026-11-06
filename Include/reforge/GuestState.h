#ifndef __TBA_REFORGE_GUESTSTATE_H
#define __TBA_REFORGE_GUESTSTATE_H

/*
 * GuestState.h
 *
 * Guest architectural state, shared by the translator and the runtime.
 * The translated code addresses every field by a fixed offset from the state
 * pointer (r15), so the layout is an ABI: change it only together with the
 * lowering in dismount and the runtime. The offsets are checked at compile
 * time below.
 */

#include <cstddef>
#include "xStl/types.h"

namespace reforge {

/* Number of IR temporaries that have a spill slot in the guest state. */
const uint64 GUEST_STATE_TEMPORARIES = 128;

struct GuestState
{
    /* 0x000: integer registers; x[0] is hard-wired to zero. */
    uint64 x[32];
    /* 0x100: guest program counter (guest virtual address). */
    uint64 pc;
    /* 0x108: host address minus guest address for guest memory (r14). */
    uint64 bias;
    /* 0x110: why translated code returned to the dispatcher. */
    uint64 exitReason;
    /* 0x118: extra information for the exit reason (e.g. trap cause). */
    uint64 trapInfo;
    /* 0x120-0x1ff: unused, keeps the temporaries at a fixed offset. */
    uint8 reserved[0x200 - 0x120];
    /* 0x200: IR temporaries spill slots. */
    uint64 temps[GUEST_STATE_TEMPORARIES];
};

static_assert(offsetof(GuestState, x) == 0x000, "GuestState.x offset");
static_assert(offsetof(GuestState, pc) == 0x100, "GuestState.pc offset");
static_assert(offsetof(GuestState, bias) == 0x108, "GuestState.bias offset");
static_assert(offsetof(GuestState, exitReason) == 0x110, "GuestState.exitReason offset");
static_assert(offsetof(GuestState, trapInfo) == 0x118, "GuestState.trapInfo offset");
static_assert(offsetof(GuestState, temps) == 0x200, "GuestState.temps offset");
static_assert(sizeof(GuestState) == 0x200 + GUEST_STATE_TEMPORARIES * 8,
              "GuestState size");

} // namespace reforge

#endif // __TBA_REFORGE_GUESTSTATE_H
