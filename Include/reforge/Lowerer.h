#ifndef __TBA_REFORGE_LOWERER_H
#define __TBA_REFORGE_LOWERER_H

/*
 * Lowerer.h
 *
 * The interface between the translator and the instruction selection of the
 * target (dismount's AMD64 lowering and assembler). The translator only needs
 * the encoded bytes of every guest block, the labels the bytes define and the
 * labels they refer to; everything else (legalizing, pattern selection,
 * register mapping, encoding) stays in dismount.
 *
 * Label contract:
 *   - the code of the guest block at VA 'a' defines the label
 *     blockLabelName(a) at offset 0 (the block linker also defines it);
 *   - a jump or call to another guest block is a Rel32 dependency on
 *     blockLabelName(target);
 *   - leaving translated code (a dynamic control transfer, a trap) is a Rel32
 *     dependency on EXIT_LABEL, which the block linker defines.
 *
 * A Rel32 dependency names the offset (inside the block's bytes) of a 4-byte
 * field that holds target - (offset + 4) once linked.
 */

#include <string>
#include <vector>
#include "xStl/types.h"

class SemanticBlock;

namespace reforge {

/* Name of the label the exit stub defines (exit of the translated code). */
extern const char* const EXIT_LABEL;
/* Name of the label of the entry stub (reforge_enter). */
extern const char* const ENTER_LABEL;

/* "B_" followed by the guest VA as lower case hex without 0x or padding (the dismount AMD64Lowering::getBlockLabel() form). */
std::string blockLabelName(uint64 guestVA);

/* A label defined by the bytes of a block. */
struct LabelDef
{
    std::string name;
    /* Offset inside the bytes of the block. */
    uint64 offset;
    LabelDef() : offset(0) {}
    LabelDef(const std::string& n, uint64 o) : name(n), offset(o) {}
};

/* How a dependency is patched. */
enum class LabelDepKind
{
    /* 4-byte little-endian signed displacement, relative to the end of the field. */
    Rel32
};

/* A reference of the bytes of a block to a label. */
struct LabelDep
{
    std::string name;
    /* Offset inside the bytes of the block of the field to patch. */
    uint64 offset;
    LabelDepKind kind;
    LabelDep() : offset(0), kind(LabelDepKind::Rel32) {}
    LabelDep(const std::string& n, uint64 o, LabelDepKind k = LabelDepKind::Rel32)
        : name(n), offset(o), kind(k) {}
};

/* The encoded AMD64 code of one guest block. */
struct LoweredBlock
{
    /* Guest VA of the first instruction of the block. */
    uint64 guestAddress;
    std::vector<uint8> code;
    std::vector<LabelDef> defs;
    std::vector<LabelDep> deps;
    LoweredBlock() : guestAddress(0) {}
};

/*
 * Turns the lifted IR of a guest block into encoded host code: legalize()
 * then lowerBlock().
 * Implementations throw ReforgeError for blocks they cannot lower.
 */
class ILowerer
{
public:
    virtual ~ILowerer() {}

    /* Rewrites the lifted block in place for the target (dismount's
     * SemanticLegalizer). The default does nothing. */
    virtual void legalize(SemanticBlock&) {}

    /* Lowers the lifted, legalized block that starts at guestAddress. */
    virtual void lowerBlock(const SemanticBlock& ir, uint64 guestAddress,
                            LoweredBlock& result) = 0;
};

} // namespace reforge

#endif // __TBA_REFORGE_LOWERER_H
