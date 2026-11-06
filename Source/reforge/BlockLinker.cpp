/*
 * BlockLinker.cpp
 *
 * See BlockLinker.h. The stubs are encoded by hand (a fixed, tiny sequence that
 * is part of the runtime ABI of Meta.h), not by the lowering.
 */
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <sstream>
#include "xStl/data/string.h"
#include "xStl/data/sarray.h"
#include "dismount/proc/ia32/AMD64Lowering.h"
#include "reforge/BlockLinker.h"
#include "reforge/Diagnostics.h"
#include "reforge/GuestState.h"

namespace reforge {

const char* const EXIT_LABEL = "__reforge_exit";
const char* const ENTER_LABEL = "reforge_enter";

std::string blockLabelName(uint64 guestVA)
{
    // The lowering refers to these labels and the linker defines them, so the
    // name is dismount's AMD64Lowering::getBlockLabel(), not a copy of it.
    cString label = AMD64Lowering::getBlockLabel(guestVA);
    cSArray<char> ascii = label.getASCIIstring();
    return std::string(ascii.getBuffer());
}

std::vector<uint8> entryStubBytes()
{
    // state offset of 'bias' is 0x108 (GuestState.h)
    static const uint8 code[] = {
        0x53,                                     // push rbx
        0x55,                                     // push rbp
        0x41, 0x54,                               // push r12
        0x41, 0x55,                               // push r13
        0x41, 0x56,                               // push r14
        0x41, 0x57,                               // push r15
        0x49, 0x89, 0xFF,                         // mov r15, rdi      (state)
        0x4D, 0x8B, 0xB7, 0x08, 0x01, 0x00, 0x00, // mov r14, [r15+0x108] (bias)
        0xFF, 0xE6                                // jmp rsi           (host target)
    };
    static_assert(offsetof(GuestState, bias) == 0x108, "entry stub encodes the bias offset");
    return std::vector<uint8>(code, code + sizeof(code));
}

std::vector<uint8> exitStubBytes()
{
    static const uint8 code[] = {
        0x41, 0x5F,                               // pop r15
        0x41, 0x5E,                               // pop r14
        0x41, 0x5D,                               // pop r13
        0x41, 0x5C,                               // pop r12
        0x5D,                                     // pop rbp
        0x5B,                                     // pop rbx
        0xC3                                      // ret
    };
    return std::vector<uint8>(code, code + sizeof(code));
}

namespace {

const uint64 BLOCK_ALIGNMENT = 16;

uint64 alignUp(uint64 value, uint64 alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

std::string hex(uint64 value)
{
    std::ostringstream s;
    s << "0x" << std::hex << value;
    return s.str();
}

[[noreturn]] void internal(const std::string& message)
{
    throw ReforgeError(RC_INTERNAL, "block linker: " + message);
}

typedef std::map<std::string, uint64> LabelMap;

void defineLabel(LabelMap& labels, const std::string& name, uint64 offset)
{
    std::pair<LabelMap::iterator, bool> inserted =
        labels.insert(std::make_pair(name, offset));
    if (!inserted.second && inserted.first->second != offset)
        internal("label '" + name + "' is defined twice (offsets " +
                 hex(inserted.first->second) + " and " + hex(offset) + ")");
}

} // anonymous namespace

LinkedCode linkBlocks(const std::vector<LoweredBlock>& blocks)
{
    LinkedCode linked;
    LabelMap labels;

    std::vector<uint8>& text = linked.text;
    std::vector<uint8> enter = entryStubBytes();
    std::vector<uint8> leave = exitStubBytes();

    text.insert(text.end(), enter.begin(), enter.end());
    linked.enterSize = enter.size();
    defineLabel(labels, ENTER_LABEL, 0);

    text.resize(alignUp(text.size(), BLOCK_ALIGNMENT), 0xCC);
    linked.exitOffset = text.size();
    defineLabel(labels, EXIT_LABEL, linked.exitOffset);
    text.insert(text.end(), leave.begin(), leave.end());

    // Place the blocks; the label offsets become absolute.
    std::vector<uint64> starts(blocks.size());
    for (std::size_t i = 0; i < blocks.size(); i++)
    {
        const LoweredBlock& block = blocks[i];
        if (block.code.empty())
            internal("block at guest " + hex(block.guestAddress) + " has no code");

        text.resize(alignUp(text.size(), BLOCK_ALIGNMENT), 0xCC);
        starts[i] = text.size();
        text.insert(text.end(), block.code.begin(), block.code.end());

        defineLabel(labels, blockLabelName(block.guestAddress), starts[i]);
        for (std::size_t d = 0; d < block.defs.size(); d++)
        {
            if (block.defs[d].offset > block.code.size())
                internal("label '" + block.defs[d].name + "' of the block at guest " +
                         hex(block.guestAddress) + " is outside its code");
            defineLabel(labels, block.defs[d].name, starts[i] + block.defs[d].offset);
        }

        ReforgeBlockEntry entry;
        entry.guestVA = block.guestAddress;
        entry.hostOffset = starts[i];
        linked.blocks.push_back(entry);
    }

    // Resolve the dependencies.
    for (std::size_t i = 0; i < blocks.size(); i++)
    {
        const LoweredBlock& block = blocks[i];
        for (std::size_t d = 0; d < block.deps.size(); d++)
        {
            const LabelDep& dep = block.deps[d];
            if (dep.kind != LabelDepKind::Rel32)
                internal("unsupported dependency kind for label '" + dep.name + "'");
            if (dep.offset > block.code.size() || block.code.size() - dep.offset < 4)
                internal("dependency on label '" + dep.name + "' of the block at guest " +
                         hex(block.guestAddress) + " lies outside its code");

            LabelMap::const_iterator target = labels.find(dep.name);
            if (target == labels.end())
                internal("unresolved label '" + dep.name + "' referenced by the block at guest " +
                         hex(block.guestAddress) + " (offset " + hex(dep.offset) + ")");

            uint64 field = starts[i] + dep.offset;
            int64 rel = static_cast<int64>(target->second) - static_cast<int64>(field + 4);
            if (rel < INT32_MIN || rel > INT32_MAX)
                internal("displacement to label '" + dep.name + "' does not fit rel32");
            uint32 value = static_cast<uint32>(static_cast<int32>(rel));
            for (int b = 0; b < 4; b++)
                text[static_cast<std::size_t>(field + b)] = static_cast<uint8>(value >> (8 * b));
        }
    }

    std::sort(linked.blocks.begin(), linked.blocks.end(),
              [](const ReforgeBlockEntry& a, const ReforgeBlockEntry& b) {
                  return a.guestVA < b.guestVA;
              });
    return linked;
}

} // namespace reforge
