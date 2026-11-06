#ifndef __TBA_REFORGE_BLOCKLINKER_H
#define __TBA_REFORGE_BLOCKLINKER_H

/*
 * BlockLinker.h
 *
 * Concatenates the encoded blocks behind the entry and exit stubs, resolves
 * the label dependencies and builds the guest VA -> host offset block table.
 *
 * Layout of the code ("reforge_text"):
 *   0x00  reforge_enter   entry stub (see Meta.h, section 1)
 *   ....  __reforge_exit  exit stub (restores the callee-saved registers, ret)
 *   ....  blocks, 16-byte aligned, padded with int3, in the order given
 */

#include <vector>
#include "xStl/types.h"
#include "reforge/Lowerer.h"
#include "reforge/Meta.h"

namespace reforge {

struct LinkedCode
{
    /* The whole code section. */
    std::vector<uint8> text;
    /* Size of the entry stub in bytes (reforge_enter is at offset 0). */
    uint64 enterSize;
    /* Offset of the exit stub. */
    uint64 exitOffset;
    /* Block table sorted by ascending guest VA; hostOffset is relative to text. */
    std::vector<ReforgeBlockEntry> blocks;
    LinkedCode() : enterSize(0), exitOffset(0) {}
};

/* Bytes of reforge_enter / the exit stub. */
std::vector<uint8> entryStubBytes();
std::vector<uint8> exitStubBytes();

/*
 * Links the blocks. Throws ReforgeError(RC_INTERNAL) naming the label for an
 * unresolved or duplicate label, a field outside the block, a displacement
 * that does not fit 32 bits or two blocks with the same guest address.
 */
LinkedCode linkBlocks(const std::vector<LoweredBlock>& blocks);

} // namespace reforge

#endif // __TBA_REFORGE_BLOCKLINKER_H
