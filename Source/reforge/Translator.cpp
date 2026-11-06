/*
 * Translator.cpp
 *
 * The translation driver (see Translator.h). The stages are plain functions so
 * that the integration of the AMD64 lowering touches one of them only.
 */
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <sys/stat.h>
#include <cstdlib>
#include <map>
#include <vector>
#include "xStl/types.h"
#include "xStl/data/sarray.h"
#include "xStl/data/string.h"
#include "xStl/stream/memoryStream.h"
#include "xStl/exceptions.h"
#include "dismount/OpcodeSubsystems.h"
#include "dismount/DefaultOpcodeDataFormatter.h"
#include "dismount/ProcessorAddress.h"
#include "dismount/StreamDisassemblerFactory.h"
#include "dismount/semantics/SemanticBlock.h"
#include "reforge/BlockLinker.h"
#include "reforge/Diagnostics.h"
#include "reforge/FlowRecovery.h"
#include "reforge/HostLinker.h"
#include "reforge/MetaBuilder.h"
#include "reforge/ObjectEmitter.h"
#include "reforge/PeLoader.h"
#include "reforge/Translator.h"

namespace reforge {

namespace {

std::string hex(uint64 value)
{
    std::ostringstream s;
    s << "0x" << std::hex << value;
    return s.str();
}

/* A dump goes to <dumpDir>/<name>.txt, or to stderr without --dump-dir. */
void writeDump(const Options& options, const std::string& name, const std::string& text)
{
    if (options.dumpDir.empty())
    {
        std::cerr << "=== " << name << " ===\n" << text << std::flush;
        return;
    }
    mkdir(options.dumpDir.c_str(), 0755);
    std::string path = options.dumpDir + "/" + name + ".txt";
    std::ofstream file(path.c_str(), std::ios::trunc);
    file << text;
    file.close();
    if (!file)
        throw ReforgeError(RC_INTERNAL, "cannot write the dump '" + path + "'");
}

std::string importsDump(const GuestImage& image)
{
    std::ostringstream s;
    const uint64 thunkBase = thunkAreaVA(image);
    for (std::size_t i = 0; i < image.imports.size(); i++)
    {
        const GuestImport& import = image.imports[i];
        AdapterId adapter = findAdapterId(import.module.c_str(), import.symbol.c_str());
        s << import.module << "!" << import.symbol << " -> adapter "
          << static_cast<unsigned>(adapter) << ", iat " << hex(import.iatVA)
          << ", thunk " << hex(thunkBase + i * REFORGE_THUNK_STRIDE) << "\n";
    }
    return s.str();
}

std::string addrmapDump(const LinkedCode& linked)
{
    std::ostringstream s;
    for (std::size_t i = 0; i < linked.blocks.size(); i++)
        s << "guest " << hex(linked.blocks[i].guestVA) << " -> text+"
          << hex(linked.blocks[i].hostOffset) << "\n";
    return s.str();
}

/*
 * Lowering stage: the lifted IR of every recovered block is legalized
 * (dismount SemanticLegalizer) and lowered to encoded AMD64 bytes (dismount
 * AMD64Lowering through ILowerer). The IR of the recovered block is copied, so
 * the FlowResult stays the lifted program.
 */
void lowerBlocks(const Options& options, const FlowResult& flow,
                 ILowerer& lowerer, std::vector<LoweredBlock>& lowered)
{
    lowered.clear();
    std::string legalizedText;
    for (std::size_t i = 0; i < flow.blocks.size(); i++)
    {
        const FlowBlock& block = flow.blocks[i];
        if (!block.ir)
            throw ReforgeError(RC_INTERNAL, "flow recovery kept no IR for the block at " +
                                            hex(block.address));
        SemanticBlock ir(*block.ir);
        lowerer.legalize(ir);
        if (options.dumpLegalized)
            legalizedText += "; block " + hex(block.address) + "\n" + formatIr(ir);

        LoweredBlock result;
        lowerer.lowerBlock(ir, block.address, result);
        lowered.push_back(result);
    }
    if (options.dumpLegalized)
        writeDump(options, "legalized", legalizedText);
}

const char* hexDigits = "0123456789abcdef";

std::string hexBytes(const uint8* bytes, std::size_t count)
{
    std::string s;
    for (std::size_t i = 0; i < count; i++)
    {
        if (i)
            s += ' ';
        s += hexDigits[bytes[i] >> 4];
        s += hexDigits[bytes[i] & 15];
    }
    return s;
}

std::string toStd(const cString& string)
{
    cSArray<char> ascii = string.getASCIIstring();
    return std::string(ascii.getBuffer());
}

/* The guest VA of a label "B_<hex>", or false. */
bool labelGuest(const std::string& name, uint64& va)
{
    if (name.size() < 3 || name[0] != 'B' || name[1] != '_')
        return false;
    char* end = 0;
    va = std::strtoull(name.c_str() + 2, &end, 16);
    return end && *end == 0;
}

/*
 * --dump-target: the generated code of every block, re-disassembled with the
 * dismount AMD64 disassembler from the linked text (so the branch targets are
 * the final ones). Each run of AMD64 instructions is preceded by the guest
 * instruction it was generated from, which gives the guest -> AMD64 mapping.
 */
std::string targetDump(const FlowResult& flow, const std::vector<LoweredBlock>& lowered,
                       const LinkedCode& linked)
{
    std::map<uint64, const FlowBlock*> flowByVA;
    for (std::size_t i = 0; i < flow.blocks.size(); i++)
        flowByVA[flow.blocks[i].address] = &flow.blocks[i];
    std::map<uint64, uint64> hostByVA;
    for (std::size_t i = 0; i < linked.blocks.size(); i++)
        hostByVA[linked.blocks[i].guestVA] = linked.blocks[i].hostOffset;

    std::ostringstream out;
    out << "; AMD64 code of reforge_text; offsets are relative to reforge_text\n";
    for (std::size_t b = 0; b < lowered.size(); b++)
    {
        const LoweredBlock& block = lowered[b];
        const FlowBlock& fb = *flowByVA[block.guestAddress];
        const uint64 base = hostByVA[block.guestAddress];

        // offset in the block -> guest VA of the instruction that starts there
        std::map<uint64, uint64> starts;
        starts[0] = block.guestAddress;
        for (std::size_t d = 0; d < block.defs.size(); d++)
        {
            uint64 va = 0;
            if (labelGuest(block.defs[d].name, va))
                starts[block.defs[d].offset] = va;
        }
        std::map<uint64, std::string> depAt; // field offset -> label
        for (std::size_t d = 0; d < block.deps.size(); d++)
            depAt[block.deps[d].offset] = block.deps[d].name;

        out << "block guest " << hex(block.guestAddress) << " -> text+" << hex(base)
            << " (" << block.code.size() << " bytes)\n";

        cBuffer buffer;
        buffer.changeSize(static_cast<uint>(block.code.size()));
        for (std::size_t i = 0; i < block.code.size(); i++)
            buffer[static_cast<uint>(i)] = linked.text[static_cast<std::size_t>(base) + i];
        cMemoryStream stream(buffer);
        StreamDisassemblerPtr disassembler = StreamDisassemblerFactory::disassemble(
            OpcodeSubsystems::DISASSEMBLER_AMD_64,
            BasicInputPtr(&stream, SMARTPTR_DESTRUCT_NONE), true,
            ProcessorAddress(ProcessorAddress::PROCESSOR_64, base), false);
        DefaultOpcodeDataFormatter formatter(1);
        uint64 offset = 0;
        while (offset < block.code.size())
        {
            std::map<uint64, uint64>::const_iterator start = starts.find(offset);
            if (start != starts.end())
            {
                std::string text;
                for (std::size_t k = 0; k < fb.instructions.size(); k++)
                    if (fb.instructions[k] == start->second)
                        text = fb.instructionText[k];
                out << "  ; guest " << hex(start->second) << ": " << text << "\n";
            }
            OpcodePtr opcode;
            try
            {
                opcode = disassembler->next();
            }
            catch (const cException&)
            {
                throw ReforgeError(RC_INTERNAL, "the generated code at text+" +
                                   hex(base + offset) + " cannot be disassembled");
            }
            const cBuffer& raw = opcode->getOpcode();
            std::string line = toStd(disassembler->getOpcodeFormat(opcode, formatter)->string());
            std::string note;
            for (uint64 k = offset; k < offset + raw.getSize(); k++)
            {
                std::map<uint64, std::string>::const_iterator dep = depAt.find(k);
                if (dep != depAt.end())
                    note = "   ; -> " + dep->second;
            }
            std::vector<uint8> bytes(raw.getSize());
            for (uint k = 0; k < raw.getSize(); k++)
                bytes[k] = raw[k];
            char prefix[32];
            snprintf(prefix, sizeof(prefix), "  %08llx: ", (unsigned long long)(base + offset));
            out << prefix << hexBytes(bytes.data(), bytes.size());
            for (std::size_t pad = bytes.size(); pad < 12; pad++)
                out << "   ";
            out << "  " << line << note << "\n";
            offset += raw.getSize();
        }
    }
    return out.str();
}

} // anonymous namespace

namespace {

/* Removes the files a failed (or not yet started) translation must not leave. */
void removeOutputs(const Options& options)
{
    if (!options.output.empty() && !options.noLink)
    {
        std::remove(options.output.c_str());
        if (options.emitObject.empty())
            std::remove((options.output + ".o").c_str());
    }
}

void translateStages(const Options& options, ILowerer* lowerer);

} // anonymous namespace

/*
 * A stale executable from an earlier run must never survive a failure (it
 * would look like a success), so the output is removed first, and again if any
 * stage throws.
 */
void translate(const Options& options, ILowerer* lowerer)
{
    removeOutputs(options);
    try
    {
        translateStages(options, lowerer);
    }
    catch (...)
    {
        removeOutputs(options);
        throw;
    }
}

namespace {

void translateStages(const Options& options, ILowerer* lowerer)
{
    // Stage 1: load the PE (exit 3 on malformed input).
    const GuestImage image = loadPeFile(options.input);
    if (options.verbose)
        logInfo("loaded " + options.input + ": base " + hex(image.base) + ", entry " +
                hex(image.entryVA) + ", " + std::to_string(image.sections.size()) +
                " sections, " + std::to_string(image.imports.size()) + " imports");

    // Stage 2: flow recovery and lift (exit 4 or 6).
    FlowOptions flowOptions;
    flowOptions.dumpDisasm = options.dumpDisasm;
    flowOptions.dumpIr = options.dumpIr;
    FlowResult flow;
    recoverFlow(image, flowOptions, flow);
    if (options.dumpDisasm)
        writeDump(options, "disasm", flow.disasmDump);
    if (options.dumpIr)
        writeDump(options, "ir", flow.irDump);

    // Imports must have an adapter (exit 5).
    checkImports(image);
    if (options.dumpImports)
        writeDump(options, "imports", importsDump(image));

    // Stage 3: legalize and lower.
    if (lowerer == 0)
        throw ReforgeError(RC_INTERNAL, "no ILowerer installed for the lowering stage");
    std::vector<LoweredBlock> lowered;
    lowerBlocks(options, flow, *lowerer, lowered);
    if (options.verbose)
        logInfo("lowered " + std::to_string(flow.instructionCount) +
                " guest instructions of " + std::to_string(flow.blocks.size()) +
                " blocks to AMD64");

    // Stage 4: block linker.
    LinkedCode linked = linkBlocks(lowered);
    if (options.dumpAddrmap)
        writeDump(options, "addrmap", addrmapDump(linked));
    if (options.dumpTarget)
        writeDump(options, "target", targetDump(flow, lowered, linked));

    // Stage 5: object. The metadata embeds the image, the tables and the imports.
    ObjectContents contents;
    contents.text = linked.text;
    contents.enterSize = linked.enterSize;
    contents.meta = buildMeta(image, linked.blocks);

    HostLinkConfig hostConfig = defaultHostLinkConfig();
    bool keepObject = !options.emitObject.empty() || options.noLink;
    std::string objectPath = options.emitObject;
    if (objectPath.empty() && !options.output.empty())
        objectPath = options.output + ".o";
    if (objectPath.empty())
        return; // nothing was asked for
    emitObject(options.objectWriter, contents, objectPath, hostConfig);

    // Stage 6: host link.
    if (!options.noLink)
    {
        linkExecutable(hostConfig, objectPath, options.output);
        if (!keepObject)
            std::remove(objectPath.c_str());
    }
}

} // anonymous namespace

} // namespace reforge
