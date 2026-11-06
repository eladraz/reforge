/*
 * TestBlockLinker.cpp
 *
 * Unit tests of the block linker and the metadata builder: rel32 patching in
 * both directions, the block table, the stubs, and the internal errors for
 * unresolved or duplicate labels.
 */

#include <functional>
#include <string>
#include <vector>
#include "xStl/types.h"
#include "xStl/except/trace.h"
#include "xStl/except/assert.h"
#include "xStl/data/string.h"
#include "xStl/data/sarray.h"
#include "dismount/proc/ia32/AMD64Lowering.h"
#include "reforge/BlockLinker.h"
#include "reforge/Diagnostics.h"
#include "reforge/Meta.h"
#include "reforge/MetaBuilder.h"
#include "reforge/rt/Program.h"
#include "tests.h"

using namespace reforge;

namespace {

/* Runs fn; returns the exit code of the ReforgeError it throws, or -1. */
int errorCode(const std::function<void()>& fn, std::string& message)
{
    try
    {
        fn();
    }
    catch (const ReforgeError& e)
    {
        message = e.what();
        return e.getExitCode();
    }
    return -1;
}

/* A block of "jmp <target>" followed by padding bytes. */
LoweredBlock jumpBlock(uint64 va, const std::string& target, uint8 padding)
{
    LoweredBlock b;
    b.guestAddress = va;
    b.code.push_back(0xE9);
    for (int i = 0; i < 4; i++)
        b.code.push_back(0);
    b.code.push_back(padding);
    b.deps.push_back(LabelDep(target, 1));
    return b;
}

int32 rel32At(const std::vector<uint8>& text, uint64 offset)
{
    uint32 v = 0;
    for (int i = 0; i < 4; i++)
        v |= static_cast<uint32>(text[static_cast<std::size_t>(offset) + i]) << (8 * i);
    return static_cast<int32>(v);
}

class cTestBlockLinker : public cTestObject
{
public:
    void test_stubs()
    {
        std::vector<uint8> enter = entryStubBytes();
        std::vector<uint8> leave = exitStubBytes();
        TESTS_ASSERT(!enter.empty() && !leave.empty());
        TESTS_ASSERT_EQUAL(enter.back(), 0xE6);  // jmp rsi
        TESTS_ASSERT_EQUAL(leave.back(), 0xC3);  // ret

        LinkedCode linked = linkBlocks(std::vector<LoweredBlock>());
        TESTS_ASSERT_EQUAL(linked.enterSize, enter.size());
        TESTS_ASSERT(linked.exitOffset >= linked.enterSize);
        TESTS_ASSERT_EQUAL(linked.exitOffset % 16, 0u);
        TESTS_ASSERT(linked.blocks.empty());
        TESTS_ASSERT_EQUAL(linked.text[0], enter[0]);
        TESTS_ASSERT_EQUAL(linked.text[static_cast<std::size_t>(linked.exitOffset)], leave[0]);
    }

    void test_resolveAndTable()
    {
        // Given in descending guest order; each jumps to the other one and to the exit.
        LoweredBlock hi = jumpBlock(0x140002000ull, blockLabelName(0x140001000ull), 0x90);
        LoweredBlock lo = jumpBlock(0x140001000ull, blockLabelName(0x140002000ull), 0x90);
        lo.code.push_back(0xE9);
        for (int i = 0; i < 4; i++)
            lo.code.push_back(0);
        lo.deps.push_back(LabelDep(EXIT_LABEL, lo.code.size() - 4));
        // An extra label inside the block that nobody uses must be accepted.
        lo.defs.push_back(LabelDef("lo_inner", 5));

        std::vector<LoweredBlock> blocks;
        blocks.push_back(hi);
        blocks.push_back(lo);
        LinkedCode linked = linkBlocks(blocks);

        TESTS_ASSERT_EQUAL(linked.blocks.size(), 2u);
        TESTS_ASSERT_EQUAL(linked.blocks[0].guestVA, 0x140001000ull);
        TESTS_ASSERT_EQUAL(linked.blocks[1].guestVA, 0x140002000ull);
        uint64 loHost = linked.blocks[0].hostOffset;
        uint64 hiHost = linked.blocks[1].hostOffset;
        TESTS_ASSERT(loHost != hiHost);
        TESTS_ASSERT_EQUAL(loHost % 16, 0u);
        TESTS_ASSERT_EQUAL(hiHost % 16, 0u);
        // hi was placed first (given first): backward jump from lo, forward from hi.
        TESTS_ASSERT(hiHost < loHost);

        TESTS_ASSERT_EQUAL(linked.text[static_cast<std::size_t>(hiHost)], 0xE9);
        TESTS_ASSERT_EQUAL(static_cast<int64>(hiHost) + 5 + rel32At(linked.text, hiHost + 1),
                           static_cast<int64>(loHost));
        TESTS_ASSERT_EQUAL(static_cast<int64>(loHost) + 5 + rel32At(linked.text, loHost + 1),
                           static_cast<int64>(hiHost));
        TESTS_ASSERT_EQUAL(static_cast<int64>(loHost) + 11 + rel32At(linked.text, loHost + 7),
                           static_cast<int64>(linked.exitOffset));
        TESTS_ASSERT(rel32At(linked.text, loHost + 1) < 0);
        TESTS_ASSERT(rel32At(linked.text, hiHost + 1) > 0);
    }

    /*
     * Guest instructions without IR (nop, fence) have no label: the lowering
     * defines labels only for instructions with code. A block start is defined
     * by the linker, so a jump to a block whose lowering defined no label at all
     * links, with the IR-less instructions in the middle and at the end of the
     * blocks. A label the lowering does define (an instruction with code) is
     * resolved like any other.
     */
    void test_irLessGuestInstructions()
    {
        // 0x1000: nop, addi (3 bytes of code), nop; falls through to 0x1010.
        LoweredBlock first;
        first.guestAddress = 0x1000;
        first.code.assign(3, 0x90);
        first.code.push_back(0xE9);
        for (int i = 0; i < 4; i++)
            first.code.push_back(0);
        first.deps.push_back(LabelDep(blockLabelName(0x1010), 4));

        // 0x1010: fence, then a jump back to 0x1000 (no lowering labels at all).
        LoweredBlock second = jumpBlock(0x1010, blockLabelName(0x1000), 0x90);

        // 0x1020: a lowered instruction at offset 2 (label defined by the lowering),
        // referenced by the block after it.
        LoweredBlock third;
        third.guestAddress = 0x1020;
        third.code.assign(2, 0x90);
        third.code.push_back(0xE9);
        for (int i = 0; i < 4; i++)
            third.code.push_back(0);
        third.defs.push_back(LabelDef(blockLabelName(0x1024), 2));
        third.deps.push_back(LabelDep(EXIT_LABEL, third.code.size() - 4));

        LoweredBlock fourth = jumpBlock(0x1030, blockLabelName(0x1024), 0x90);

        std::vector<LoweredBlock> blocks;
        blocks.push_back(first);
        blocks.push_back(second);
        blocks.push_back(third);
        blocks.push_back(fourth);
        LinkedCode linked = linkBlocks(blocks);
        TESTS_ASSERT_EQUAL(linked.blocks.size(), 4u);

        uint64 host0 = linked.blocks[0].hostOffset;
        uint64 host1 = linked.blocks[1].hostOffset;
        uint64 host2 = linked.blocks[2].hostOffset;
        uint64 host3 = linked.blocks[3].hostOffset;
        TESTS_ASSERT_EQUAL(linked.blocks[1].guestVA, 0x1010ull);
        // first -> second: the jump at offset 4 of the block
        TESTS_ASSERT_EQUAL(static_cast<int64>(host0) + 8 + rel32At(linked.text, host0 + 4),
                           static_cast<int64>(host1));
        // second -> first
        TESTS_ASSERT_EQUAL(static_cast<int64>(host1) + 5 + rel32At(linked.text, host1 + 1),
                           static_cast<int64>(host0));
        // fourth -> the lowered label at offset 2 of third
        TESTS_ASSERT_EQUAL(static_cast<int64>(host3) + 5 + rel32At(linked.text, host3 + 1),
                           static_cast<int64>(host2 + 2));
    }

    void test_errors()
    {
        std::string message;
        std::vector<LoweredBlock> blocks;
        blocks.push_back(jumpBlock(0x1000, "B_missing_target", 0x90));
        TESTS_ASSERT_EQUAL(errorCode([&]() { linkBlocks(blocks); }, message), (int)RC_INTERNAL);
        TESTS_ASSERT(message.find("unresolved label 'B_missing_target'") != std::string::npos);

        std::vector<LoweredBlock> same;
        same.push_back(jumpBlock(0x1000, EXIT_LABEL, 0x90));
        same.push_back(jumpBlock(0x1000, EXIT_LABEL, 0x90));
        TESTS_ASSERT_EQUAL(errorCode([&]() { linkBlocks(same); }, message), (int)RC_INTERNAL);
        TESTS_ASSERT(message.find("defined twice") != std::string::npos);

        LoweredBlock bad = jumpBlock(0x1000, EXIT_LABEL, 0x90);
        bad.deps[0].offset = 4; // field would end past the code
        std::vector<LoweredBlock> outside(1, bad);
        TESTS_ASSERT_EQUAL(errorCode([&]() { linkBlocks(outside); }, message), (int)RC_INTERNAL);
        TESTS_ASSERT(message.find("outside its code") != std::string::npos);

        LoweredBlock empty;
        empty.guestAddress = 0x2000;
        std::vector<LoweredBlock> none(1, empty);
        TESTS_ASSERT_EQUAL(errorCode([&]() { linkBlocks(none); }, message), (int)RC_INTERNAL);
    }

    void test_meta()
    {
        GuestImage image;
        image.base = 0x140000000ull;
        image.sizeOfImage = 0x2000;
        image.entryVA = image.base + 0x1000;
        image.image.assign(0x2000, 0);
        GuestImport printfImport;
        printfImport.module = "msvcrt.dll";
        printfImport.symbol = "printf";
        printfImport.iatVA = image.base + 0x600;
        image.imports.push_back(printfImport);

        std::vector<ReforgeBlockEntry> blocks(1);
        blocks[0].guestVA = image.entryVA;
        blocks[0].hostOffset = 0x40;
        std::vector<uint8> meta = buildMeta(image, blocks);

        // The runtime must accept what the translator builds.
        rt::Program program = rt::Program::parse(meta.data(), meta.size());
        TESTS_ASSERT_EQUAL(program.imports.size(), 1u);
        TESTS_ASSERT_EQUAL(program.imports[0].module, std::string("msvcrt.dll"));
        TESTS_ASSERT_EQUAL(program.imports[0].symbol, std::string("printf"));
        TESTS_ASSERT_EQUAL((int)program.imports[0].adapter, (int)ADAPTER_MSVCRT_PRINTF);
        TESTS_ASSERT_EQUAL(program.thunkVA, thunkAreaVA(image));
        uint64 host = 0;
        TESTS_ASSERT(program.lookupBlock(image.entryVA, host));
        TESTS_ASSERT_EQUAL(host, 0x40u);
        // The IAT slot of the embedded image holds the thunk VA.
        uint64 slot = 0;
        for (int i = 7; i >= 0; i--)
            slot = (slot << 8) | program.image[0x600 + i];
        TESTS_ASSERT_EQUAL(slot, program.imports[0].thunkVA);

        // Unknown import -> exit 5 naming it.
        GuestImage unknown = image;
        unknown.imports[0].symbol = "Sleep";
        unknown.imports[0].module = "kernel32.dll";
        std::string message;
        TESTS_ASSERT_EQUAL(errorCode([&]() { buildMeta(unknown, blocks); }, message),
                           (int)RC_UNRESOLVED_IMPORT);
        TESTS_ASSERT(message.find("kernel32.dll!Sleep") != std::string::npos);

        // IAT slot outside the image is an internal error.
        GuestImage outside = image;
        outside.imports[0].iatVA = image.base + 0x1FFC;
        TESTS_ASSERT_EQUAL(errorCode([&]() { buildMeta(outside, blocks); }, message),
                           (int)RC_INTERNAL);
    }

    void test_labelName()
    {
        // The linker's block labels are dismount's: the lowering refers to
        // them by AMD64Lowering::getBlockLabel() and must match the definitions.
        const uint64 vas[] = { 0x140001000ull, 0x1000ull, 0xdeadbeefull, 0 };
        for (std::size_t i = 0; i < sizeof(vas) / sizeof(vas[0]); i++)
        {
            cString dismount = AMD64Lowering::getBlockLabel(vas[i]);
            cSArray<char> ascii = dismount.getASCIIstring();
            TESTS_ASSERT_EQUAL(blockLabelName(vas[i]), std::string(ascii.getBuffer()));
        }
        TESTS_ASSERT_EQUAL(blockLabelName(0x140001000ull), std::string("B_140001000"));
        TESTS_ASSERT_EQUAL(blockLabelName(0xabcull), std::string("B_abc"));
    }

    void test_exitReasons()
    {
        // The values the lowering writes and the runtime reads (see the
        // static_asserts in Amd64Lowerer.cpp for the link between them).
        TESTS_ASSERT_EQUAL(REFORGE_EXIT_DISPATCH, 1u);
        TESTS_ASSERT_EQUAL(REFORGE_EXIT_CALL, 2u);
        TESTS_ASSERT_EQUAL(REFORGE_EXIT_RETURN, 3u);
        TESTS_ASSERT_EQUAL(REFORGE_EXIT_TRAP, 4u);
    }

    virtual void test()
    {
        test_stubs();
        test_resolveAndTable();
        test_errors();
        test_meta();
        test_irLessGuestInstructions();
        test_labelName();
        test_exitReasons();
    }

    virtual cString getName() { return __FILE__; }
};

cTestBlockLinker g_globalTestBlockLinker;

} // anonymous namespace
