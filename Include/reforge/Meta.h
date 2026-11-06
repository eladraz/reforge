#ifndef __TBA_REFORGE_META_H
#define __TBA_REFORGE_META_H

/*
 * Meta.h
 *
 * Shared contract between the translator (Source/reforge, which emits the
 * image) and the runtime (Runtime/, which is linked into the translated
 * executable). Only plain constants, PODs and inline helpers live here, so the
 * runtime library does not need to link against the translator.
 *
 * =============================================================================
 * 1. Symbols exported by the translated object (ELF64 ET_REL, host ABI)
 * =============================================================================
 *
 *   reforge_meta   (global, .rodata.reforge, 8-byte aligned)
 *       The metadata blob described in section 2. The runtime reads it with
 *       the header fields only, so its total size is stored in the blob.
 *
 *   reforge_text   (global, .text.reforge, start of the translated code)
 *       Host code offsets in the block table are relative to this symbol.
 *
 *   reforge_enter  (global function, SysV: void reforge_enter(GuestState* st,
 *                   const void* hostTarget))
 *       Entry stub. It saves the callee-saved host registers, sets r15 = st,
 *       r14 = st->bias and jumps to hostTarget. Control comes back to the
 *       caller when translated code jumps to the exit stub, which restores the
 *       callee-saved registers and returns. Translated code never calls host
 *       functions and never uses the host stack.
 *
 * The runtime references the three symbols, so the translated object and
 * libreforge_rt.a are linked into one executable by the host C++ compiler.
 *
 * =============================================================================
 * 2. Metadata blob (reforge_meta)
 * =============================================================================
 *
 * All integers are little-endian. Every offset is measured from the first byte
 * of the blob. Fields are naturally aligned, the header is 0x80 bytes long.
 *
 *   0x00 uint32 magic            REFORGE_META_MAGIC ('R','F','G','M' in memory)
 *   0x04 uint32 version          REFORGE_META_VERSION (must match exactly)
 *   0x08 uint64 totalSize        size of the whole blob in bytes
 *   0x10 uint64 guestBase        preferred guest VA of image byte 0
 *   0x18 uint64 sizeOfImage      bytes of the image (the PE SizeOfImage)
 *   0x20 uint64 entryVA          guest VA of the entry point
 *   0x28 uint64 regionSize       bytes of guest memory the runtime allocates,
 *                                starting at guestBase (image, thunks, stack)
 *   0x30 uint64 thunkVA          guest VA of the thunk area (thunk i is at
 *                                thunkVA + 16*i); lies after the image
 *   0x38 uint64 stackTopVA       initial guest sp (16-byte aligned); the stack
 *                                is the REFORGE_STACK_SIZE bytes below it
 *   0x40 uint64 imageOffset      offset of the image bytes in the blob
 *                                (sizeOfImage bytes, laid out by RVA)
 *   0x48 uint64 blockCount       number of entries in the block table
 *   0x50 uint64 blockTableOffset offset of the block table
 *   0x58 uint64 importCount      number of entries in the import table
 *   0x60 uint64 importTableOffset offset of the import table
 *   0x68 uint64 stringsOffset    offset of the NUL-terminated name strings
 *   0x70 uint64 stringsSize      size of the string area in bytes
 *   0x78 uint64 reserved         zero
 *
 * Block table: blockCount entries of BlockEntry (16 bytes each), sorted by
 * strictly ascending guestVA. hostOffset is relative to reforge_text. Every
 * guest block leader and every call return site is an entry. Guest pc values
 * that are not in the table are rejected by the runtime.
 *
 * Import table: importCount entries of ImportEntry (24 bytes each). Entry i is
 * the import whose IAT slot holds thunkVA + 16*i. moduleOffset and
 * symbolOffset point at NUL-terminated strings inside the string area. The
 * module name is lower case (e.g. "msvcrt.dll"). adapterId is an AdapterId
 * from section 4, or ADAPTER_NONE (never valid for the runtime).
 *
 * Image: the bytes of the guest image laid out by RVA, zero filled, with the
 * IAT slots already holding the thunk VAs. The runtime copies the bytes to
 * offset 0 of the guest region.
 *
 * Guest memory layout (set up by the runtime from the blob):
 *   [guestBase, guestBase + sizeOfImage)     image, read/write data
 *   [thunkVA, thunkVA + 16*importCount)      thunks, NOT accessible to data
 *                                            accesses (they are pc targets)
 *   [stackTopVA - REFORGE_STACK_SIZE, stackTopVA)  stack
 *   the whole region is [guestBase, guestBase + regionSize)
 *
 * =============================================================================
 * 3. Guest state and exit protocol
 * =============================================================================
 *
 * The runtime sets the state fields (GuestState.h) and calls reforge_enter:
 *   pc = entryVA, x[1] (ra) = REFORGE_HOST_RETURN_PC, x[2] (sp) = stackTopVA,
 *   bias = hostRegion - guestBase.
 * Translated code exits with exitReason set:
 *   REFORGE_EXIT_DISPATCH/CALL/RETURN  state.pc holds the next guest pc to look up
 *   REFORGE_EXIT_TRAP      state.pc is the trapping instruction, exitReason
 *                          holds the trap kind and number; the runtime reports it
 * Guest pc == REFORGE_HOST_RETURN_PC means the entry function returned; the
 * process status is x[10] (a0) & 0xff.
 *
 * Import call convention (guest): an import call is
 *   auipc t0, %pcrel_hi(__imp_X); ld t0, %pcrel_lo(...)(t0); jalr ra, 0(t0)
 * so the runtime sees pc == thunk VA with ra = call site + 4. The call site
 * (ra - 4) is the address reported in diagnostics.
 *
 * =============================================================================
 * 4. Adapter ids
 * =============================================================================
 *
 * Adapters are keyed by (module, symbol). The table below is the single source
 * of that mapping; the translator uses findAdapterId() to fill ImportEntry and
 * the runtime uses the same id to pick the host function. Unknown imports are
 * translate-time errors, so ADAPTER_NONE never reaches the runtime.
 */

// The standard headers come before xStl/types.h: xStl defines the macro uint,
// which clashes with the sys/types.h typedef if that header is read later.
#include <cstddef>
#include <cstring>
#include <string>
#include "xStl/types.h"

namespace reforge {

/* 'R' 'F' 'G' 'M' as bytes in the blob, read as a little-endian uint32. */
const uint32 REFORGE_META_MAGIC = 0x4D474652u;
const uint32 REFORGE_META_VERSION = 1;
/* Size of the header; all tables follow it. */
const uint64 REFORGE_META_HEADER_SIZE = 0x80;
/* Bytes per entry of the block table and of the import thunk area. */
const uint64 REFORGE_BLOCK_ENTRY_SIZE = 16;
const uint64 REFORGE_IMPORT_ENTRY_SIZE = 24;
const uint64 REFORGE_THUNK_STRIDE = 16;
/* Size of the guest stack reserved below stackTopVA. */
const uint64 REFORGE_STACK_SIZE = 1024 * 1024;
/* Guest pc that means "the entry function returned to the host". */
const uint64 REFORGE_HOST_RETURN_PC = 0xFFFFFFFFFFFF0000ull;
/* Process exit status of a runtime fault (bad guest access, unsupported
 * printf conversion, unresolved guest pc, ...). */
const int REFORGE_RUNTIME_FAULT_EXIT = 70;

/* GuestState.exitReason values written by translated code. They are the
 * AMD64Lowering::ExitReason values of dismount: the low byte is the reason;
 * for a trap bits 8-15 are the SemanticOperation::TrapKind and bits 16-63 the
 * trap number. DISPATCH (a branch), CALL (a dynamic call, ra already set) and
 * RETURN all mean "state.pc holds the next guest pc to look up". */
const uint64 REFORGE_EXIT_DISPATCH = 1;
const uint64 REFORGE_EXIT_CALL = 2;
const uint64 REFORGE_EXIT_RETURN = 3;
const uint64 REFORGE_EXIT_TRAP = 4;
const uint64 REFORGE_EXIT_REASON_MASK = 0xff;

/* Adapter ids, stored in ImportEntry.adapterId. */
enum AdapterId : uint32
{
    ADAPTER_NONE = 0,
    /* msvcrt.dll!printf: literal text, %s and %% only. */
    ADAPTER_MSVCRT_PRINTF = 1,
    /* kernel32.dll!ExitProcess: terminates the runtime with the status. */
    ADAPTER_KERNEL32_EXITPROCESS = 2
};

/* One row of the (module, symbol) -> adapter table. */
struct AdapterKeyEntry
{
    const char* module;
    const char* symbol;
    AdapterId id;
};

/* The table of implemented adapters. Modules are compared case-insensitively. */
inline const AdapterKeyEntry* adapterKeyTable(std::size_t& count)
{
    static const AdapterKeyEntry table[] = {
        { "msvcrt.dll", "printf", ADAPTER_MSVCRT_PRINTF },
        { "kernel32.dll", "ExitProcess", ADAPTER_KERNEL32_EXITPROCESS },
    };
    count = sizeof(table) / sizeof(table[0]);
    return table;
}

/* ASCII case-insensitive string equality (portable, no locale). */
inline bool metaEqualsIgnoreCase(const char* a, const char* b)
{
    for (;; a++, b++)
    {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (ca != cb)
            return false;
        if (ca == '\0')
            return true;
    }
}

/* Returns the adapter id for a guest import, or ADAPTER_NONE. */
inline AdapterId findAdapterId(const char* module, const char* symbol)
{
    std::size_t count = 0;
    const AdapterKeyEntry* table = adapterKeyTable(count);
    for (std::size_t i = 0; i < count; i++)
    {
        if (metaEqualsIgnoreCase(table[i].module, module) &&
            std::strcmp(table[i].symbol, symbol) == 0)
            return table[i].id;
    }
    return ADAPTER_NONE;
}

/* Header of reforge_meta (layout in section 2). */
struct ReforgeMetaHeader
{
    uint32 magic;
    uint32 version;
    uint64 totalSize;
    uint64 guestBase;
    uint64 sizeOfImage;
    uint64 entryVA;
    uint64 regionSize;
    uint64 thunkVA;
    uint64 stackTopVA;
    uint64 imageOffset;
    uint64 blockCount;
    uint64 blockTableOffset;
    uint64 importCount;
    uint64 importTableOffset;
    uint64 stringsOffset;
    uint64 stringsSize;
    uint64 reserved;
};

/* One entry of the block table. */
struct ReforgeBlockEntry
{
    uint64 guestVA;
    uint64 hostOffset;
};

/* One entry of the import table. */
struct ReforgeImportEntry
{
    uint32 adapterId;
    uint32 moduleOffset;
    uint32 symbolOffset;
    uint32 reserved;
    uint64 iatVA;
};

static_assert(sizeof(ReforgeMetaHeader) == REFORGE_META_HEADER_SIZE,
              "ReforgeMetaHeader must match the documented layout");
static_assert(sizeof(ReforgeBlockEntry) == REFORGE_BLOCK_ENTRY_SIZE,
              "ReforgeBlockEntry must match the documented layout");
static_assert(sizeof(ReforgeImportEntry) == REFORGE_IMPORT_ENTRY_SIZE,
              "ReforgeImportEntry must match the documented layout");

} // namespace reforge

#endif // __TBA_REFORGE_META_H
