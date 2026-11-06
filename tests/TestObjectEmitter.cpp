/*
 * TestObjectEmitter.cpp
 *
 * End-to-end test of the translator back half without the lowering: hand
 * encoded AMD64 blocks that follow the runtime contract (Meta.h, section 3)
 * go through the block linker, the metadata builder, the object emitter (both
 * the elf library writer and the .S + .incbin fallback) and the host linker;
 * the resulting executables are run and their output and status are checked.
 * Section flags of the object and the program headers of the executable are
 * checked with readelf (no writable and executable section or segment).
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>
#include "xStl/types.h"
#include "xStl/except/trace.h"
#include "xStl/except/assert.h"
#include "reforge/BlockLinker.h"
#include "reforge/Diagnostics.h"
#include "reforge/GuestState.h"
#include "reforge/HostLinker.h"
#include "reforge/MetaBuilder.h"
#include "reforge/ObjectEmitter.h"
#include "tests.h"

using namespace reforge;

namespace {

const uint64 BASE = 0x140000000ull;
const uint64 STATE_PC = 0x100;
const uint64 STATE_EXIT_REASON = 0x110;

/* Appends "mov rax, imm64; mov [r15+off], rax" (sets a 64-bit state field). */
void setState(LoweredBlock& b, uint64 off, uint64 value)
{
    b.code.push_back(0x48); b.code.push_back(0xB8);
    for (int i = 0; i < 8; i++)
        b.code.push_back(static_cast<uint8>(value >> (8 * i)));
    b.code.push_back(0x49); b.code.push_back(0x89); b.code.push_back(0x87);
    for (int i = 0; i < 4; i++)
        b.code.push_back(static_cast<uint8>(off >> (8 * i)));
}

/* Appends "jmp <label>" with a rel32 dependency. */
void jump(LoweredBlock& b, const std::string& label)
{
    b.code.push_back(0xE9);
    b.deps.push_back(LabelDep(label, b.code.size()));
    for (int i = 0; i < 4; i++)
        b.code.push_back(0);
}

/* Leaves the translated code with the next guest pc (dispatcher request). */
void dispatchTo(LoweredBlock& b, uint64 pc)
{
    setState(b, STATE_PC, pc);
    setState(b, STATE_EXIT_REASON, REFORGE_EXIT_DISPATCH);
    jump(b, EXIT_LABEL);
}

LoweredBlock newBlock(uint64 va)
{
    LoweredBlock b;
    b.guestAddress = va;
    return b;
}

/*
 * The guest program: A prints "hello-world\n" through the printf import and
 * "returns" to C; C returns 3 to the host. B only exists to be jumped over
 * (a forward and a backward label reference).
 */
struct Program
{
    GuestImage image;
    std::vector<LoweredBlock> blocks;
};

Program makeProgram()
{
    Program p;
    p.image.base = BASE;
    p.image.sizeOfImage = 0x2000;
    p.image.entryVA = BASE + 0x1000;
    p.image.image.assign(0x2000, 0);
    const char text[] = "hello-world\n";
    std::memcpy(&p.image.image[0x800], text, sizeof(text));
    GuestImport printfImport;
    printfImport.module = "msvcrt.dll";
    printfImport.symbol = "printf";
    printfImport.iatVA = BASE + 0x600;
    p.image.imports.push_back(printfImport);

    const uint64 vaA = BASE + 0x1000;
    const uint64 vaB = BASE + 0x1020;
    const uint64 vaC = BASE + 0x1040;
    const uint64 thunk = thunkAreaVA(p.image);

    // Block order in the code: C, A, B (so A -> B is forward, B -> C backward).
    LoweredBlock c = newBlock(vaC);
    setState(c, 10 * 8, 3);                      // a0 = 3
    dispatchTo(c, REFORGE_HOST_RETURN_PC);

    LoweredBlock a = newBlock(vaA);
    setState(a, 10 * 8, BASE + 0x800);           // a0 = format string
    setState(a, 1 * 8, vaC);                     // ra = return site (the block C)
    jump(a, blockLabelName(vaB));

    LoweredBlock b = newBlock(vaB);
    dispatchTo(b, thunk);                        // call the printf thunk

    p.blocks.push_back(c);
    p.blocks.push_back(a);
    p.blocks.push_back(b);
    return p;
}

std::string tempDirectory()
{
    const char* base = std::getenv("TMPDIR");
    std::string pattern = std::string(base && *base ? base : "/tmp") + "/reforge-test-XXXXXX";
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    if (mkdtemp(&buffer[0]) == 0)
        throw ReforgeError(RC_INTERNAL, "cannot create a temporary directory");
    return &buffer[0];
}

int errorCode(const HostLinkConfig& config, const std::string& object, const std::string& out)
{
    try
    {
        linkExecutable(config, object, out);
    }
    catch (const ReforgeError& e)
    {
        return e.getExitCode();
    }
    return -1;
}

class cTestObjectEmitter : public cTestObject
{
public:
    /* Section flags: "AX" for the text, "A" for the metadata, never W together with X. */
    void checkObject(const std::string& object)
    {
        ProcessResult r = runProcess(std::vector<std::string>{ "readelf", "-SW", object });
        if (r.exitCode != 0)
        {
            std::printf("(readelf unavailable, section flag check skipped) ");
            return;
        }
        bool sawText = false, sawRodata = false;
        std::size_t pos = 0;
        while (pos < r.out.size())
        {
            std::size_t end = r.out.find('\n', pos);
            if (end == std::string::npos) end = r.out.size();
            std::string line = r.out.substr(pos, end - pos);
            pos = end + 1;
            std::size_t name = line.find("] ");
            std::size_t flagsPos = std::string::npos;
            if (line.find(".text.reforge") != std::string::npos)
            {
                sawText = true;
                flagsPos = line.find(" AX ");
                TESTS_ASSERT(flagsPos != std::string::npos);
            }
            else if (line.find(".rodata.reforge") != std::string::npos)
            {
                sawRodata = true;
                TESTS_ASSERT(line.find(" A ") != std::string::npos);
            }
            (void)name;
            // No allocated section may be both writable and executable.
            TESTS_ASSERT(line.find("WAX") == std::string::npos);
            TESTS_ASSERT(line.find(" AWX ") == std::string::npos);
            TESTS_ASSERT(line.find(" WAX ") == std::string::npos);
        }
        TESTS_ASSERT(sawText && sawRodata);
    }

    void checkExecutable(const std::string& exe)
    {
        ProcessResult r = runProcess(std::vector<std::string>{ "readelf", "-lW", exe });
        if (r.exitCode != 0)
            return;
        TESTS_ASSERT(r.out.find("RWE") == std::string::npos);
        ProcessResult h = runProcess(std::vector<std::string>{ "readelf", "-hW", exe });
        TESTS_ASSERT(h.out.find("Advanced Micro Devices X86-64") != std::string::npos);
    }

    void buildAndRun(ObjectWriter writer, const std::string& dir, const std::string& tag)
    {
        Program p = makeProgram();
        LinkedCode linked = linkBlocks(p.blocks);
        ObjectContents contents;
        contents.text = linked.text;
        contents.enterSize = linked.enterSize;
        contents.meta = buildMeta(p.image, linked.blocks);

        HostLinkConfig config = defaultHostLinkConfig();
        std::string object = dir + "/" + tag + ".o";
        std::string exe = dir + "/" + tag + ".exe";
        emitObject(writer, contents, object, config);
        checkObject(object);
        linkExecutable(config, object, exe);
        checkExecutable(exe);

        ProcessResult run = runProcess(std::vector<std::string>{ exe });
        TESTS_ASSERT_EQUAL(run.out, std::string("hello-world\n"));
        TESTS_ASSERT_EQUAL(run.err, std::string());
        TESTS_ASSERT_EQUAL(run.exitCode, 3);

        std::remove(object.c_str());
        std::remove(exe.c_str());
    }

    void test_hostReturn(const std::string& dir)
    {
        // Entry block that returns 42 to the host with no import at all.
        GuestImage image;
        image.base = BASE;
        image.sizeOfImage = 0x2000;
        image.entryVA = BASE + 0x1000;
        image.image.assign(0x2000, 0);
        LoweredBlock a = newBlock(image.entryVA);
        setState(a, 10 * 8, 42);
        dispatchTo(a, REFORGE_HOST_RETURN_PC);
        std::vector<LoweredBlock> blocks(1, a);
        LinkedCode linked = linkBlocks(blocks);

        ObjectContents contents;
        contents.text = linked.text;
        contents.enterSize = linked.enterSize;
        contents.meta = buildMeta(image, linked.blocks);
        HostLinkConfig config = defaultHostLinkConfig();
        std::string object = dir + "/ret42.o";
        std::string exe = dir + "/ret42.exe";
        writeElfObject(contents, object);
        linkExecutable(config, object, exe);
        ProcessResult run = runProcess(std::vector<std::string>{ exe });
        TESTS_ASSERT_EQUAL(run.out, std::string());
        TESTS_ASSERT_EQUAL(run.exitCode, 42);
        std::remove(object.c_str());
        std::remove(exe.c_str());
    }

    void test_linkFailures(const std::string& dir)
    {
        std::string object = dir + "/fail.o";
        std::string exe = dir + "/fail.exe";
        Program p = makeProgram();
        LinkedCode linked = linkBlocks(p.blocks);
        ObjectContents contents;
        contents.text = linked.text;
        contents.enterSize = linked.enterSize;
        contents.meta = buildMeta(p.image, linked.blocks);
        writeElfObject(contents, object);

        // Without the runtime the symbol main is undefined -> exit 7, no output file.
        HostLinkConfig noRuntime = defaultHostLinkConfig();
        noRuntime.libraries.clear();
        TESTS_ASSERT_EQUAL(errorCode(noRuntime, object, exe), (int)RC_LINK_FAILURE);
        TESTS_ASSERT(access(exe.c_str(), F_OK) != 0);

        // A missing host compiler is a link failure too.
        HostLinkConfig noCompiler = defaultHostLinkConfig();
        noCompiler.compiler = "reforge-no-such-compiler";
        TESTS_ASSERT_EQUAL(errorCode(noCompiler, object, exe), (int)RC_LINK_FAILURE);
        std::remove(object.c_str());
    }

    virtual void test()
    {
        std::string dir = tempDirectory();
        try
        {
            buildAndRun(ObjectWriter::ELFLIB, dir, "elflib");
            buildAndRun(ObjectWriter::ASM, dir, "asm");
            test_hostReturn(dir);
            test_linkFailures(dir);
        }
        catch (...)
        {
            rmdir(dir.c_str());
            throw;
        }
        rmdir(dir.c_str());
    }

    virtual cString getName() { return __FILE__; }
};

cTestObjectEmitter g_globalTestObjectEmitter;

} // anonymous namespace
