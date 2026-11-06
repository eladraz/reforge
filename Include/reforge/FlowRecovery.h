#ifndef __TBA_REFORGE_FLOWRECOVERY_H
#define __TBA_REFORGE_FLOWRECOVERY_H

/*
 * FlowRecovery.h
 *
 * Static recovery of the control flow of a guest image (RV64I). Starting at
 * the entry point, every reachable instruction is decoded and lifted with the
 * dismount RISC-V decoder and semantic lifter, the basic blocks are split at
 * the branch targets and the return sites, and each indirect call is
 * classified by a local constant propagation over the IR of its block:
 *
 *   - a call through a value loaded from an import address table slot
 *     (auipc / ld / jalr of an import call) is an import call;
 *   - a direct call (jal) is a call to a constant address;
 *   - a return (jalr x0, 0(ra)) is allowed, the dispatcher checks it;
 *   - any other indirect branch or call is unresolved control flow.
 *
 * Rejections, both reported as ReforgeError (see Diagnostics.h):
 *   RC_UNSUPPORTED_INSTRUCTION (4) - a reachable instruction that cannot be
 *       decoded or lifted, an OP_UNDEFINED or an OP_TRAP (ecall, ebreak...),
 *       naming the guest address and the instruction text.
 *   RC_UNRESOLVED_CONTROL_FLOW (6) - an indirect branch or call which is not
 *       an import call or a return, or a flow which leaves the executable
 *       sections of the image, naming the guest address.
 */

#include <memory>
#include <string>
#include <vector>
#include "xStl/types.h"
#include "reforge/GuestImage.h"

class SemanticBlock;

namespace reforge {

/* A call of an import: the guest address of the call instruction and the
 * index of the import in GuestImage::imports. */
struct FlowImportCall
{
    uint64 site;
    uint importIndex;
};

/* A basic block of the guest program. */
struct FlowBlock
{
    /* VA of the first instruction (a leader). */
    uint64 address;
    /* VA just after the last instruction. */
    uint64 end;
    /* VA of each instruction of the block, in order. */
    std::vector<uint64> instructions;
    /* Text of each instruction (dismount formatter), parallel to
     * 'instructions'. */
    std::vector<std::string> instructionText;
    /* The lifted IR of the whole block (RISC-V 64 architecture, one guest
     * instruction per beginGuestInstruction entry, fall-through address =
     * 'end'). Not yet legalized: the translator copies it before legalizing. */
    std::shared_ptr<SemanticBlock> ir;
    /* VAs of the blocks that control can reach from the end of this block
     * (direct branch targets, the fall-through and the return site of a
     * call). Empty for a return. */
    std::vector<uint64> successors;
    /* True when the last instruction is an import call. */
    bool endsWithImportCall;
};

struct FlowOptions
{
    /* Fill FlowResult::disasmDump. */
    bool dumpDisasm;
    /* Fill FlowResult::irDump. */
    bool dumpIr;
    FlowOptions() : dumpDisasm(false), dumpIr(false) {}
};

struct FlowResult
{
    /* Blocks sorted by address. */
    std::vector<FlowBlock> blocks;
    /* Import calls sorted by site. */
    std::vector<FlowImportCall> importCalls;
    /* Number of reachable guest instructions. */
    uint64 instructionCount;
    /* Text dump of the blocks and their instructions (dismount formatter). */
    std::string disasmDump;
    /* Text dump of the semantic IR of each instruction (SemanticPrinter). */
    std::string irDump;

    FlowResult() : instructionCount(0) {}
};

/* The SemanticPrinter text of a block lifted from the RISC-V 64 guest (a
 * legalized block still has guest registers), one line per IR instruction
 * with its guest address. */
std::string formatIr(const SemanticBlock& block);

/*
 * Recover the flow of the image from its entry point. Throws ReforgeError
 * (exit 4 or 6, see above) on a rejected program. The image is only read.
 */
void recoverFlow(const GuestImage& image, const FlowOptions& options,
                 FlowResult& result);

} // namespace reforge

#endif // __TBA_REFORGE_FLOWRECOVERY_H
