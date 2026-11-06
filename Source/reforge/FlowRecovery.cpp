/*
 * FlowRecovery.cpp
 *
 * Control flow recovery of the guest RV64I image. See FlowRecovery.h.
 *
 * Two passes over the reachable code:
 *   1. Discovery: from the entry point, each reachable instruction is decoded
 *      (dismount RiscvStreamDisassembler::decode, the rv64i features) and
 *      lifted (SemanticLifterFactory, RISCV_64). The terminators give the
 *      leaders: the direct branch targets, the fall-throughs and the return
 *      sites of the calls. A trap or an undefined instruction is rejected
 *      here (exit 4), since it is reachable.
 *   2. Blocks: the blocks are cut at the leaders. A forward local constant
 *      propagation over the IR of a block (the registers, the temporaries of
 *      one instruction) classifies each indirect call: a value loaded from
 *      the import address table slot of an import is an import call.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include "xStl/types.h"
#include "xStl/data/smartptr.h"
#include "xStl/data/sarray.h"
#include "xStl/stream/memoryStream.h"
#include "xStl/stream/stringerStream.h"
#include "dismount/OpcodeSubsystems.h"
#include "dismount/DefaultOpcodeDataFormatter.h"
#include "dismount/ProcessorAddress.h"
#include "dismount/semantics/Semantics.h"
#include "dismount/semantics/SemanticPrinter.h"
#include "dismount/proc/riscv/RiscvInstructionSet.h"
#include "dismount/proc/riscv/RiscvFeatures.h"
#include "dismount/proc/riscv/RiscvOpcode.h"
#include "dismount/proc/riscv/RiscvStreamDisassembler.h"
#include "reforge/FlowRecovery.h"
#include "reforge/Diagnostics.h"

namespace reforge {

namespace {

/* The value of a register, a temporary or a load, in the local constant
 * propagation. */
struct Value
{
    enum Kind { UNKNOWN, CONST, IMPORT };

    Kind kind;
    /* The value of a CONST. */
    uint64 value;
    /* The import of an IMPORT: the value loaded from its IAT slot. */
    uint importIndex;

    Value() : kind(UNKNOWN), value(0), importIndex(0) {}

    static Value constant(uint64 v)
    {
        Value result;
        result.kind = CONST;
        result.value = v;
        return result;
    }

    static Value importSlot(uint index)
    {
        Value result;
        result.kind = IMPORT;
        result.importIndex = index;
        return result;
    }
};

/* How an instruction transfers control. */
enum Terminator
{
    TERM_NONE,
    TERM_BRANCH,
    TERM_CONDITIONAL,
    TERM_CALL,
    TERM_RETURN,
    TERM_INDIRECT
};

/* A decoded and lifted reachable instruction. */
struct Insn
{
    uint64 address;
    uint size;
    uint32 encoding;
    RiscvOpcodePtr opcode;
    std::string text;
    Terminator term;
    /* The target of a direct branch or call. */
    bool hasDirectTarget;
    uint64 directTarget;
    /* The operand of an indirect call or branch (valid when not direct). */
    SemanticOperand target;
    /* The IR of the instruction. Its temporaries are local to it. */
    SemanticBlock ir;

    Insn(uint64 va)
        : address(va), size(0), encoding(0), term(TERM_NONE), hasDirectTarget(false),
          directTarget(0),
          ir(OpcodeSubsystems::DISASSEMBLER_RISCV_64)
    {
    }
};

std::string hex64(uint64 value)
{
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "0x%llx", (unsigned long long)value);
    return buffer;
}

std::string toStd(const cString& string)
{
    cSArray<char> ascii = string.getASCIIstring();
    return std::string(ascii.getBuffer());
}

// The opcode as an OpcodePtr which does not own it (the RiscvOpcodePtr owns)
OpcodePtr asOpcode(const RiscvOpcodePtr& opcode)
{
    return OpcodePtr((Opcode*)opcode.getPointer(), SMARTPTR_DESTRUCT_NONE);
}

// Collects the text of SemanticPrinter
class TextCapture : public cStringerStream {
public:
    TextCapture() : cStringerStream(false) {}

    virtual void outputString(const cString& string)
    {
        m_text += toStd(string);
    }

    std::string m_text;
};

class FlowBuilder {
public:
    FlowBuilder(const GuestImage& image)
        : m_image(image),
          m_lifter(SemanticLifterFactory::getLifter(
                       OpcodeSubsystems::DISASSEMBLER_RISCV_64)),
          m_formatter(RiscvInstructionSet::RISCV_RV64,
                      BasicInputPtr(&m_empty, SMARTPTR_DESTRUCT_NONE), true,
                      ProcessorAddress(ProcessorAddress::PROCESSOR_64, 0),
                      false, features())
    {
    }

    void run(FlowResult& result, const FlowOptions& options)
    {
        if (!isExecutable(m_image.entryVA, 2))
            throw ReforgeError(RC_MALFORMED_INPUT,
                               "entry point " + hex64(m_image.entryVA) +
                               " is not inside an executable section");

        discover(m_image.entryVA);
        buildBlocks(result);
        result.instructionCount = m_insns.size();

        if (options.dumpDisasm)
            dumpDisasm(result);
        if (options.dumpIr)
            dumpIr(result);
    }

private:
    // The guest memory: the image laid out by RVA, based at the image base
    bool isExecutable(uint64 va, uint length) const
    {
        if (va < m_image.base || length == 0)
            return false;
        uint64 rva = va - m_image.base;
        if (rva + length < rva || rva + length > m_image.image.size())
            return false;
        for (size_t i = 0; i < m_image.sections.size(); i++)
        {
            const GuestSection& section = m_image.sections[i];
            if (section.executable && rva >= section.rva &&
                rva + length <= section.rva + section.virtualSize)
                return true;
        }
        return false;
    }

    uint64 readLE(uint64 va, uint length) const
    {
        uint64 rva = va - m_image.base;
        uint64 value = 0;
        for (uint i = 0; i < length; i++)
            value |= (uint64)m_image.image[rva + i] << (8 * i);
        return value;
    }

    // The target features: RV64I only
    static const RiscvFeatureSet& features()
    {
        static RiscvFeatureSet set;
        static bool parsed = set.parse("rv64i");
        if (!parsed)
            throw ReforgeError(RC_INTERNAL, "cannot parse the rv64i features");
        return set;
    }

    // The dismount text of an instruction (the formatter reads no stream)
    std::string formatText(const RiscvOpcodePtr& opcode)
    {
        DefaultOpcodeDataFormatter formatter(1);
        return toStd(m_formatter.getOpcodeFormat(asOpcode(opcode),
                                                 formatter)->string());
    }

    Insn* decodeAt(uint64 va)
    {
        std::map<uint64, std::unique_ptr<Insn> >::iterator it = m_insns.find(va);
        if (it != m_insns.end())
            return it->second.get();

        // RV64I without the C extension: IALIGN is 32, so every instruction
        // and every control flow target is 4-byte aligned.
        if ((va & 3) != 0)
            throw ReforgeError(RC_UNRESOLVED_CONTROL_FLOW,
                               "unresolved control flow: misaligned target at " +
                               hex64(va) + " (RV64I instructions are 4-byte aligned)");

        if (!isExecutable(va, 2))
            throw ReforgeError(RC_UNRESOLVED_CONTROL_FLOW,
                               "control flow reaches " + hex64(va) +
                               " which is outside the executable sections");

        uint16 parcel = (uint16)readLE(va, 2);
        uint size = RiscvInstructionSet::getInstructionSize(parcel);
        if (size != 2 && size != 4)
            throw ReforgeError(RC_UNSUPPORTED_INSTRUCTION,
                               "unsupported instruction (longer than 32 bits) at " +
                               hex64(va));
        if (!isExecutable(va, size))
            throw ReforgeError(RC_UNRESOLVED_CONTROL_FLOW,
                               "instruction at " + hex64(va) +
                               " crosses the end of the executable section");

        uint32 encoding = (uint32)readLE(va, size);
        RiscvOpcodePtr opcode = RiscvStreamDisassembler::decode(
            RiscvInstructionSet::RISCV_RV64, encoding, size, va, features());
        if (opcode.getPointer() == NULL)
        {
            char bits[32];
            snprintf(bits, sizeof(bits), "0x%08x", (unsigned)encoding);
            throw ReforgeError(RC_UNSUPPORTED_INSTRUCTION,
                               "unsupported instruction " + std::string(bits) +
                               (size == 2 ? " (compressed or invalid 16-bit encoding; "
                                            "only RV64I is supported)"
                                          : " (invalid encoding or not in RV64I)") +
                               " at " + hex64(va));
        }

        std::unique_ptr<Insn> insn(new Insn(va));
        insn->size = size;
        insn->encoding = encoding;
        insn->opcode = opcode;
        insn->text = formatText(opcode);
        insn->ir.setFallThroughAddress(ProcessorAddress(
            ProcessorAddress::PROCESSOR_64, va + size));

        bool lifted = m_lifter->lift(asOpcode(opcode), insn->ir);
        classify(*insn);
        // dismount lifts "jalr x0, 0(x1 or x5)" as a return. Only the return
        // through ra goes to the dispatcher (the instruction after a call is
        // always a block start); a jump through x5 is an unresolved branch.
        if (insn->term == TERM_RETURN && !isPlainRet(encoding))
            throw ReforgeError(RC_UNRESOLVED_CONTROL_FLOW,
                               "unresolved control flow at " + hex64(va) +
                               ": indirect branch '" + insn->text +
                               "' (only 'ret' through ra is supported)");
        if (!lifted || hasUndefined(insn->ir))
            throw ReforgeError(RC_UNSUPPORTED_INSTRUCTION,
                               "unsupported instruction '" + insn->text +
                               "' at " + hex64(va));
        if (hasTrap(insn->ir))
            throw ReforgeError(RC_UNSUPPORTED_INSTRUCTION,
                               "unsupported instruction '" + insn->text +
                               "' at " + hex64(va) +
                               " (system call or trap)");

        Insn* result = insn.get();
        m_insns[va] = std::move(insn);
        return result;
    }

    // jalr x0, 0(ra)
    static bool isPlainRet(uint32 encoding)
    {
        return encoding == 0x00008067u;
    }

    static bool hasUndefined(const SemanticBlock& block)
    {
        const cList<SemanticInstruction>& ops = block.getInstructions();
        for (auto it = ops.begin(); it != ops.end(); ++it)
            if ((*it).getOperation() == SemanticOperation::OP_UNDEFINED)
                return true;
        return false;
    }

    static bool hasTrap(const SemanticBlock& block)
    {
        const cList<SemanticInstruction>& ops = block.getInstructions();
        for (auto it = ops.begin(); it != ops.end(); ++it)
            if ((*it).getOperation() == SemanticOperation::OP_TRAP)
                return true;
        return false;
    }

    static bool isDirect(const SemanticOperand& operand)
    {
        return operand.getKind() == SemanticOperand::KIND_IMMEDIATE ||
               operand.getKind() == SemanticOperand::KIND_GUEST_ADDRESS;
    }

    void classify(Insn& insn)
    {
        const cList<SemanticInstruction>& ops = insn.ir.getInstructions();
        for (auto it = ops.begin(); it != ops.end(); ++it)
        {
            const SemanticInstruction& op = *it;
            switch (op.getOperation())
            {
            case SemanticOperation::OP_BRANCH:
                insn.term = TERM_BRANCH;
                insn.target = op.getSource(0);
                break;
            case SemanticOperation::OP_BRANCH_CONDITIONAL:
                insn.term = TERM_CONDITIONAL;
                insn.target = op.getSource(1);
                break;
            case SemanticOperation::OP_CALL:
                insn.term = TERM_CALL;
                insn.target = op.getSource(0);
                break;
            case SemanticOperation::OP_RETURN:
                insn.term = TERM_RETURN;
                break;
            case SemanticOperation::OP_BRANCH_INDIRECT:
                insn.term = TERM_INDIRECT;
                insn.target = op.getSource(0);
                break;
            default:
                break;
            }
        }
        if ((insn.term == TERM_BRANCH || insn.term == TERM_CONDITIONAL ||
             insn.term == TERM_CALL) && isDirect(insn.target))
        {
            insn.hasDirectTarget = true;
            insn.directTarget = (uint64)insn.target.getValue();
        }
    }

    void enqueue(uint64 va)
    {
        m_leaders.insert(va);
        if (m_visited.find(va) == m_visited.end())
        {
            m_visited.insert(va);
            m_work.push_back(va);
        }
    }

    void discover(uint64 entry)
    {
        enqueue(entry);
        while (!m_work.empty())
        {
            uint64 va = m_work.back();
            m_work.pop_back();

            // A straight line run up to the next terminator
            for (;;)
            {
                Insn* insn = decodeAt(va);
                if (insn->term == TERM_NONE)
                {
                    va += insn->size;
                    continue;
                }
                uint64 next = va + insn->size;
                switch (insn->term)
                {
                case TERM_BRANCH:
                    if (!insn->hasDirectTarget)
                        throw ReforgeError(RC_UNRESOLVED_CONTROL_FLOW,
                                           "unresolved control flow at " +
                                           hex64(va) + ": indirect branch");
                    enqueue(insn->directTarget);
                    break;
                case TERM_CONDITIONAL:
                    if (!insn->hasDirectTarget)
                        throw ReforgeError(RC_UNRESOLVED_CONTROL_FLOW,
                                           "unresolved control flow at " +
                                           hex64(va) + ": indirect branch");
                    enqueue(insn->directTarget);
                    enqueue(next);
                    break;
                case TERM_CALL:
                    if (insn->hasDirectTarget)
                        enqueue(insn->directTarget);
                    enqueue(next);
                    break;
                default:
                    // TERM_RETURN and TERM_INDIRECT: no successor here, the
                    // indirect call is resolved by the blocks
                    break;
                }
                break;
            }
        }
    }

    // Local constant propagation over the registers of a block. A register
    // has no value at the start of a block, except x0.
    struct RegisterState
    {
        std::vector<Value> regs;
        RegisterState() : regs(32)
        {
            regs[0] = Value::constant(0);
        }
    };

    Value valueOf(const SemanticOperand& operand, const RegisterState& state,
                  const std::vector<Value>& temps) const
    {
        switch (operand.getKind())
        {
        case SemanticOperand::KIND_REGISTER:
            if (operand.getIndex() < state.regs.size())
                return state.regs[operand.getIndex()];
            return Value();
        case SemanticOperand::KIND_TEMPORARY:
            if (operand.getIndex() < temps.size())
                return temps[operand.getIndex()];
            return Value();
        case SemanticOperand::KIND_IMMEDIATE:
        case SemanticOperand::KIND_GUEST_ADDRESS:
            return Value::constant((uint64)operand.getValue());
        default:
            return Value();
        }
    }

    static Value addValues(const Value& a, const Value& b)
    {
        if (a.kind == Value::CONST && b.kind == Value::CONST)
            return Value::constant(a.value + b.value);
        // An import slot plus zero is the slot (the jalr offset)
        if (a.kind == Value::IMPORT && b.kind == Value::CONST && b.value == 0)
            return a;
        if (b.kind == Value::IMPORT && a.kind == Value::CONST && a.value == 0)
            return b;
        return Value();
    }

    static Value andValues(const Value& a, const Value& b)
    {
        if (a.kind == Value::CONST && b.kind == Value::CONST)
            return Value::constant(a.value & b.value);
        // The import slot is a VA of the import table, so it is aligned and
        // clearing bit 0 of it (jalr) keeps it
        if (a.kind == Value::IMPORT && b.kind == Value::CONST &&
            b.value == ~(uint64)1)
            return a;
        if (b.kind == Value::IMPORT && a.kind == Value::CONST &&
            a.value == ~(uint64)1)
            return b;
        return Value();
    }

    // The import whose IAT slot is at the address, or -1
    int importAt(uint64 address) const
    {
        for (size_t i = 0; i < m_image.imports.size(); i++)
            if (m_image.imports[i].iatVA == address)
                return (int)i;
        return -1;
    }

    // Propagate the values through the IR of an instruction. The temporaries
    // of the instruction are in temps.
    void propagate(const Insn& insn, RegisterState& state,
                   std::vector<Value>& temps) const
    {
        temps.assign(insn.ir.getTemporariesCount(), Value());
        const cList<SemanticInstruction>& ops = insn.ir.getInstructions();
        for (auto it = ops.begin(); it != ops.end(); ++it)
        {
            const SemanticInstruction& op = *it;
            const SemanticOperand& dest = op.getDestination();
            if (dest.getKind() != SemanticOperand::KIND_REGISTER &&
                dest.getKind() != SemanticOperand::KIND_TEMPORARY)
                continue;

            Value result;
            switch (op.getOperation())
            {
            case SemanticOperation::OP_MOVE:
                result = valueOf(op.getSource(0), state, temps);
                break;
            case SemanticOperation::OP_ADD:
                result = addValues(valueOf(op.getSource(0), state, temps),
                                   valueOf(op.getSource(1), state, temps));
                break;
            case SemanticOperation::OP_AND:
                result = andValues(valueOf(op.getSource(0), state, temps),
                                   valueOf(op.getSource(1), state, temps));
                break;
            case SemanticOperation::OP_LOAD:
            {
                Value address = valueOf(op.getSource(0), state, temps);
                if (address.kind == Value::CONST)
                {
                    int index = importAt(address.value);
                    if (index >= 0)
                        result = Value::importSlot((uint)index);
                }
                break;
            }
            default:
                break;
            }

            if (dest.getKind() == SemanticOperand::KIND_TEMPORARY)
            {
                if (dest.getIndex() < temps.size())
                    temps[dest.getIndex()] = result;
            }
            else if (dest.getIndex() != 0 && dest.getIndex() < state.regs.size())
            {
                state.regs[dest.getIndex()] = result;
            }
        }
    }

    void buildBlocks(FlowResult& result)
    {
        for (std::set<uint64>::const_iterator leader = m_leaders.begin();
             leader != m_leaders.end(); ++leader)
        {
            FlowBlock block;
            block.address = *leader;
            block.end = *leader;
            block.endsWithImportCall = false;

            RegisterState state;
            std::vector<Value> temps;
            uint64 va = *leader;
            block.ir.reset(new SemanticBlock(
                OpcodeSubsystems::DISASSEMBLER_RISCV_64));
            for (;;)
            {
                Insn* insn = m_insns[va].get();
                block.instructions.push_back(va);
                block.instructionText.push_back(insn->text);
                // The block IR is the concatenation of the lifts of its
                // instructions (the lifter opens a guest instruction first).
                if (!m_lifter->lift(asOpcode(insn->opcode), *block.ir))
                    throw ReforgeError(RC_UNSUPPORTED_INSTRUCTION,
                                       "unsupported instruction '" + insn->text +
                                       "' at " + hex64(va));
                uint64 next = va + insn->size;
                propagate(*insn, state, temps);

                if (insn->term == TERM_NONE)
                {
                    if (m_leaders.find(next) != m_leaders.end())
                    {
                        block.successors.push_back(next);
                        block.end = next;
                        break;
                    }
                    va = next;
                    continue;
                }

                block.end = next;
                switch (insn->term)
                {
                case TERM_BRANCH:
                    block.successors.push_back(insn->directTarget);
                    break;
                case TERM_CONDITIONAL:
                    block.successors.push_back(insn->directTarget);
                    block.successors.push_back(next);
                    break;
                case TERM_CALL:
                    if (insn->hasDirectTarget)
                    {
                        block.successors.push_back(insn->directTarget);
                    }
                    else
                    {
                        Value callee = valueOf(insn->target, state, temps);
                        if (callee.kind != Value::IMPORT)
                            throw ReforgeError(RC_UNRESOLVED_CONTROL_FLOW,
                                               "unresolved control flow at " +
                                               hex64(va) +
                                               ": indirect call through an unknown target");
                        // The runtime resumes after an import call at ra, so
                        // the call must link through ra (jalr rd, 0(t0): rd).
                        if (((insn->encoding >> 7) & 0x1f) != 1)
                            throw ReforgeError(RC_UNRESOLVED_CONTROL_FLOW,
                                               "unresolved control flow at " +
                                               hex64(va) + ": import call '" +
                                               insn->text + "' does not link "
                                               "through ra (only 'jalr ra' "
                                               "import calls are supported)");
                        FlowImportCall call;
                        call.site = va;
                        call.importIndex = callee.importIndex;
                        result.importCalls.push_back(call);
                        block.endsWithImportCall = true;
                    }
                    block.successors.push_back(next);
                    break;
                case TERM_RETURN:
                    break;
                case TERM_INDIRECT:
                    throw ReforgeError(RC_UNRESOLVED_CONTROL_FLOW,
                                       "unresolved control flow at " + hex64(va) +
                                       ": indirect branch");
                default:
                    break;
                }
                break;
            }
            block.ir->setFallThroughAddress(ProcessorAddress(
                ProcessorAddress::PROCESSOR_64, block.end));
            result.blocks.push_back(block);
        }
    }

    void dumpDisasm(FlowResult& result) const
    {
        std::string text;
        for (size_t b = 0; b < result.blocks.size(); b++)
        {
            const FlowBlock& block = result.blocks[b];
            char header[128];
            snprintf(header, sizeof(header), "block %s (%u instructions)\n",
                     hex64(block.address).c_str(),
                     (unsigned)block.instructions.size());
            text += header;
            for (size_t i = 0; i < block.instructions.size(); i++)
            {
                const Insn* insn = m_insns.find(block.instructions[i])->second.get();
                text += "  " + hex64(insn->address) + ": " + insn->text + "\n";
            }
        }
        result.disasmDump = text;
    }

    void dumpIr(FlowResult& result) const
    {
        ArchitectureDescriptionPtr architecture =
            ArchitectureDescriptionFactory::getArchitecture(
                OpcodeSubsystems::DISASSEMBLER_RISCV_64);
        std::string text;
        for (size_t b = 0; b < result.blocks.size(); b++)
        {
            const FlowBlock& block = result.blocks[b];
            text += "; block " + hex64(block.address) + "\n";
            for (size_t i = 0; i < block.instructions.size(); i++)
            {
                const Insn* insn = m_insns.find(block.instructions[i])->second.get();
                TextCapture capture;
                SemanticPrinter::print(insn->ir, *architecture, capture);
                capture.flush();
                text += capture.m_text;
            }
        }
        result.irDump = text;
    }

    const GuestImage& m_image;
    SemanticLifterPtr m_lifter;
    cMemoryStream m_empty;
    RiscvStreamDisassembler m_formatter;
    std::map<uint64, std::unique_ptr<Insn> > m_insns;
    std::set<uint64> m_leaders;
    std::set<uint64> m_visited;
    std::vector<uint64> m_work;
};

} // namespace

std::string formatIr(const SemanticBlock& block)
{
    ArchitectureDescriptionPtr architecture =
        ArchitectureDescriptionFactory::getArchitecture(
            OpcodeSubsystems::DISASSEMBLER_RISCV_64);
    TextCapture capture;
    SemanticPrinter::print(block, *architecture, capture);
    capture.flush();
    return capture.m_text;
}

void recoverFlow(const GuestImage& image, const FlowOptions& options,
                 FlowResult& result)
{
    result = FlowResult();
    FlowBuilder builder(image);
    builder.run(result, options);
}

} // namespace reforge
