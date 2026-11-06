#ifndef __TBA_REFORGE_AMD64LOWERER_H
#define __TBA_REFORGE_AMD64LOWERER_H

/*
 * Amd64Lowerer.h
 *
 * The ILowerer backed by dismount: SemanticLegalizer (RV64 -> AMD64) and
 * AMD64Lowering::lowerBlock with the Reforge register map (GuestState.h).
 * All instruction selection and encoding is dismount's; this file only maps
 * the guest state of Reforge onto the register map contract of dismount and
 * converts the result to the types of the block linker.
 */

#include "reforge/Lowerer.h"

namespace reforge {

class Amd64Lowerer : public ILowerer
{
public:
    Amd64Lowerer();
    virtual ~Amd64Lowerer();

    virtual void legalize(SemanticBlock& ir);
    virtual void lowerBlock(const SemanticBlock& legalized, uint64 guestAddress,
                            LoweredBlock& result);

private:
    struct Impl;
    Impl* m_impl;
};

} // namespace reforge

#endif // __TBA_REFORGE_AMD64LOWERER_H
