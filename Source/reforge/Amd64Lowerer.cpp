/*
 * Amd64Lowerer.cpp
 *
 * See Amd64Lowerer.h. The register map places the RISC-V registers at the
 * offsets of reforge::GuestState and the IR temporaries in its spill area.
 */
#include <string>
#include "xStl/types.h"
#include "xStl/data/string.h"
#include "xStl/data/sarray.h"
#include "xStl/exceptions.h"
#include "dismount/OpcodeSubsystems.h"
#include "dismount/semantics/ArchitectureDescription.h"
#include "dismount/semantics/SemanticBlock.h"
#include "dismount/semantics/SemanticLegalizer.h"
#include "dismount/semantics/SemanticLowering.h"
#include "dismount/proc/ia32/AMD64Lowering.h"
#include "dismount/proc/ia32/AMD64Architecture.h"
#include "dismount/proc/riscv/RiscvArchitecture.h"
#include "reforge/Amd64Lowerer.h"
#include "reforge/Diagnostics.h"
#include "reforge/GuestState.h"
#include "reforge/Meta.h"

namespace reforge {

/*
 * The exit reasons are defined twice: by dismount (the lowering writes them)
 * and by Meta.h (the runtime reads them). This translation unit sees both, so
 * a change on one side breaks the build here instead of at run time.
 */
static_assert(REFORGE_EXIT_DISPATCH == AMD64Lowering::EXIT_BRANCH,
              "a branch exit is REFORGE_EXIT_DISPATCH");
static_assert(REFORGE_EXIT_CALL == AMD64Lowering::EXIT_CALL,
              "a call exit is REFORGE_EXIT_CALL (2)");
static_assert(REFORGE_EXIT_RETURN == AMD64Lowering::EXIT_RETURN,
              "a return exit is REFORGE_EXIT_RETURN");
static_assert(REFORGE_EXIT_TRAP == AMD64Lowering::EXIT_TRAP,
              "a trap exit is REFORGE_EXIT_TRAP");

namespace {

/* Where the guest registers and the IR temporaries live: in GuestState,
 * addressed from r15; guest memory is addressed [r14 + guest address]. */
class ReforgeRegisterMap : public SemanticRegisterMap {
public:
    virtual bool getTargetRegister(uint, uint&) const { return false; }

    virtual uint getStateOffset(uint guestRegister) const
    {
        if (guestRegister >= riscvdis::RISCV_REG_X0 &&
            guestRegister <= riscvdis::RISCV_REG_X31)
            return static_cast<uint>(offsetof(GuestState, x)) +
                   (guestRegister - riscvdis::RISCV_REG_X0) * 8;
        throw ReforgeError(RC_INTERNAL, "guest register " +
                           std::to_string(guestRegister) +
                           " has no slot in the guest state");
    }

    virtual uint getStateRegister() const { return ia32dis::AMD64_REG_R15; }

    virtual uint getTemporaryOffset(uint temporary) const
    {
        if (temporary >= GUEST_STATE_TEMPORARIES)
            throw ReforgeError(RC_INTERNAL, "IR temporary " +
                               std::to_string(temporary) +
                               " is beyond the spill slots of the guest state");
        return static_cast<uint>(offsetof(GuestState, temps)) + temporary * 8;
    }

    virtual uint getMemoryBaseRegister() const { return ia32dis::AMD64_REG_R14; }
    virtual uint getProgramCounterOffset() const
    {
        return static_cast<uint>(offsetof(GuestState, pc));
    }
    virtual uint getExitReasonOffset() const
    {
        return static_cast<uint>(offsetof(GuestState, exitReason));
    }
};

std::string toStd(const cString& string)
{
    cSArray<char> ascii = string.getASCIIstring();
    return std::string(ascii.getBuffer());
}

std::string messageOf(cException& e)
{
    return toStd(cString(e.getMessage()));
}

} // anonymous namespace

struct Amd64Lowerer::Impl
{
    ArchitectureDescriptionPtr guest;
    ArchitectureDescriptionPtr target;
    SemanticLegalizer legalizer;
    AMD64Lowering lowering;
    ReforgeRegisterMap registerMap;

    Impl()
        : guest(ArchitectureDescriptionFactory::getArchitecture(
              OpcodeSubsystems::DISASSEMBLER_RISCV_64)),
          target(ArchitectureDescriptionFactory::getArchitecture(
              OpcodeSubsystems::DISASSEMBLER_AMD_64)),
          legalizer(guest, target)
    {
    }
};

Amd64Lowerer::Amd64Lowerer() : m_impl(new Impl) {}

Amd64Lowerer::~Amd64Lowerer() { delete m_impl; }

void Amd64Lowerer::legalize(SemanticBlock& ir)
{
    try
    {
        m_impl->legalizer.legalize(ir);
    }
    catch (cException& e)
    {
        throw ReforgeError(RC_UNSUPPORTED_INSTRUCTION,
                           "cannot legalize the block: " + messageOf(e));
    }
}

void Amd64Lowerer::lowerBlock(const SemanticBlock& legalized, uint64 guestAddress,
                              LoweredBlock& result)
{
    AMD64Lowering::LoweredBlock lowered;
    try
    {
        m_impl->lowering.lowerBlock(legalized, m_impl->registerMap, lowered);
    }
    catch (cException& e)
    {
        throw ReforgeError(RC_UNSUPPORTED_INSTRUCTION,
                           "cannot lower the block at guest 0x" +
                           blockLabelName(guestAddress).substr(2) +
                           ": " + messageOf(e));
    }

    result = LoweredBlock();
    result.guestAddress = guestAddress;
    result.code.resize(lowered.m_code.getSize());
    for (uint i = 0; i < lowered.m_code.getSize(); i++)
        result.code[i] = lowered.m_code[i];

    const std::string first = blockLabelName(guestAddress);
    for (auto it = lowered.m_labels.begin(); it != lowered.m_labels.end(); ++it)
    {
        std::string name = toStd((*it).m_name);
        // The block linker defines the label of the block start itself.
        if (name == first && (*it).m_offset == 0)
            continue;
        result.defs.push_back(LabelDef(name, (*it).m_offset));
    }
    for (auto it = lowered.m_dependencies.begin();
         it != lowered.m_dependencies.end(); ++it)
    {
        if ((*it).m_size != 4 ||
            (*it).m_kind != BinaryDependencies::DEP_RELATIVE)
            throw ReforgeError(RC_INTERNAL,
                               "the lowering produced a dependency that is not rel32");
        result.deps.push_back(LabelDep(toStd((*it).m_name), (*it).m_offset,
                                       LabelDepKind::Rel32));
    }
}

} // namespace reforge
