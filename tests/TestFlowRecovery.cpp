/*
 * TestFlowRecovery.cpp
 *
 * Tests of the flow recovery (reforge/FlowRecovery.h) on the RV64I PE32+
 * fixtures written by scripts/build_fixtures.sh. The fixture directory is
 * taken from the REFORGE_FIXTURES environment variable, else from the CMake
 * cache (REFORGE_FIXTURE_DIR).
 *
 * Set REFORGE_DUMP=1 to print the disassembly and the IR of the hello
 * fixture.
 */

#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <string>
#include "tests.h"
#include "reforge/Diagnostics.h"
#include "reforge/FlowRecovery.h"
#include "reforge/GuestImage.h"
#include "reforge/PeLoader.h"

using namespace reforge;

namespace {

void failTest(const std::string& message)
{
    std::cerr << "test_reforge: " << message << std::endl;
    TESTS_ASSERT(false);
}

#define EXPECT(condition)                                                     \
    do {                                                                      \
        if (!(condition))                                                     \
            failTest(std::string(__FILE__) + ":" + std::to_string(__LINE__) + \
                     ": expected " #condition);                               \
    } while (0)

std::string fixturePath(const std::string& relative)
{
    const char* dir = getenv("REFORGE_FIXTURES");
    std::string root = dir ? dir : REFORGE_FIXTURE_DIR;
    return root + "/" + relative;
}

// "module!symbol" of an import call
std::string importName(const GuestImage& image, const FlowImportCall& call)
{
    EXPECT(call.importIndex < image.imports.size());
    const GuestImport& import = image.imports[call.importIndex];
    return import.module + "!" + import.symbol;
}

// The guest address named after the last "at 0x" of a rejection message
uint64 addressInMessage(const std::string& message)
{
    size_t at = message.rfind("at 0x");
    if (at == std::string::npos)
        return 0;
    return strtoull(message.c_str() + at + 3, NULL, 16);
}

// Run the recovery on a fixture: the result is returned, and the test fails
// on a rejection
FlowResult recoverFixture(const std::string& relative, GuestImage& image,
                          const FlowOptions& options = FlowOptions())
{
    image = loadPeFile(fixturePath(relative));
    FlowResult result;
    recoverFlow(image, options, result);
    return result;
}

// The recovery must reject the fixture with the exit code, and the message
// must name the address of the offending instruction, inside the image
void expectRejected(const std::string& relative, ExitCode code,
                    const std::string& mustContain)
{
    GuestImage image = loadPeFile(fixturePath(relative));
    bool rejected = false;
    try
    {
        FlowResult result;
        recoverFlow(image, FlowOptions(), result);
    }
    catch (const ReforgeError& error)
    {
        rejected = true;
        const std::string message = error.what();
        std::cout << "  " << relative << ": exit " << error.getExitCode()
                  << ": " << message << std::endl;
        EXPECT(error.getExitCode() == code);
        EXPECT(message.find(mustContain) != std::string::npos);
        const uint64 address = addressInMessage(message);
        EXPECT(address >= image.base && address < image.base + image.sizeOfImage);
    }
    EXPECT(rejected);
}

class TestFlowRecovery : public cTestObject
{
public:
    // hello: entry block to the printf call, the return to the ExitProcess
    // call, and the halt loop. Two import calls, by IAT slot.
    static void testHello()
    {
        for (const char* name : {"hello-riscv64-windows.exe",
                                 "hello-alt-riscv64-windows.exe"})
        {
            GuestImage image;
            FlowOptions options;
            options.dumpDisasm = true;
            options.dumpIr = true;
            FlowResult result = recoverFixture(name, image, options);

            if (getenv("REFORGE_DUMP"))
                std::cout << result.disasmDump << result.irDump;

            EXPECT(result.blocks.size() == 3);
            EXPECT(result.importCalls.size() == 2);
            if (result.importCalls.size() == 2)
            {
                EXPECT(importName(image, result.importCalls[0]) == "msvcrt.dll!printf");
                EXPECT(importName(image, result.importCalls[1]) ==
                       "kernel32.dll!ExitProcess");
            }
            EXPECT(result.disasmDump.find("jalr") != std::string::npos);
            EXPECT(result.irDump.find("; guest ") != std::string::npos);
            std::cout << "  " << name << ": " << result.blocks.size()
                      << " blocks, " << result.instructionCount
                      << " instructions, " << result.importCalls.size()
                      << " import calls" << std::endl;
        }
    }

    // compute: a loop over the sum, and the same two imports
    static void testCompute()
    {
        for (const char* name : {"compute-riscv64-windows.exe",
                                 "compute-alt-riscv64-windows.exe"})
        {
            GuestImage image;
            FlowResult result = recoverFixture(name, image);
            EXPECT(result.blocks.size() > 3);
            EXPECT(result.importCalls.size() == 2);
            std::cout << "  " << name << ": " << result.blocks.size()
                      << " blocks, " << result.importCalls.size()
                      << " import calls" << std::endl;
        }
    }

    // An ecall on a reachable path: exit 4, named by its address
    static void testEcallRejected()
    {
        expectRejected("bad/ecall-riscv64-windows.exe",
                       RC_UNSUPPORTED_INSTRUCTION, "'ecall'");
    }

    // The import of an unresolved import fixture is a valid import call here;
    // the adapter lookup (exit 5) happens later, in the translator
    static void testUnresolvedImportIsAnImportCall()
    {
        GuestImage image;
        FlowResult result = recoverFixture("bad/unresolved_import-riscv64-windows.exe",
                                           image);
        EXPECT(result.importCalls.size() >= 1);
        for (size_t i = 0; i < result.importCalls.size(); i++)
            EXPECT(importName(image, result.importCalls[i]).find("kernel32.dll!") == 0);
    }

    // Control flow through a code pointer read from data (not an IAT slot) is
    // unresolved: exit 6 with the guest address of the jalr
    static void testIndirectRejected()
    {
        expectRejected("bad/indirect-call-riscv64-windows.exe",
                       RC_UNRESOLVED_CONTROL_FLOW, "indirect call");
        expectRejected("bad/indirect-jump-riscv64-windows.exe",
                       RC_UNRESOLVED_CONTROL_FLOW, "indirect branch");
    }

    // An import call that links through t0 instead of ra would resume at the
    // wrong address in the runtime: exit 6 with the guest address of the jalr
    static void testImportCallLinkRegisterRejected()
    {
        expectRejected("bad/linkreg-riscv64-windows.exe",
                       RC_UNRESOLVED_CONTROL_FLOW, "does not link through ra");
    }

    // A jump to a target that is not 4-byte aligned (RV64I without C): exit 6
    // naming the target address
    static void testMisalignedTargetRejected()
    {
        expectRejected("bad/misaligned-riscv64-windows.exe",
                       RC_UNRESOLVED_CONTROL_FLOW, "misaligned target");
    }

    // An invalid, RV64M or compressed word on the executed path: exit 4
    static void testUndecodableRejected()
    {
        expectRejected("bad/garbage-riscv64-windows.exe",
                       RC_UNSUPPORTED_INSTRUCTION, "unsupported instruction");
        expectRejected("bad/zeroword-riscv64-windows.exe",
                       RC_UNSUPPORTED_INSTRUCTION, "unsupported instruction");
        expectRejected("bad/mul-riscv64-windows.exe",
                       RC_UNSUPPORTED_INSTRUCTION, "unsupported instruction");
        expectRejected("bad/compressed-riscv64-windows.exe",
                       RC_UNSUPPORTED_INSTRUCTION, "unsupported instruction");
    }

    virtual void test()
    {
        testIndirectRejected();
        testImportCallLinkRegisterRejected();
        testMisalignedTargetRejected();
        testUndecodableRejected();
        testHello();
        testCompute();
        testEcallRejected();
        testUnresolvedImportIsAnImportCall();
    }

    virtual cString getName() { return __FILE__; }
};

TestFlowRecovery g_testFlowRecovery;

} // anonymous namespace
