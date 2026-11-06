/*
 * TestRuntimeAdapters.cpp
 *
 * Unit tests of the reforge runtime that do not need a translated program:
 *  - printf formatting (literal text, %s, %%) and rejection of other
 *    conversions, missing arguments and bad or unterminated guest strings;
 *  - guest memory bounds and forbidden ranges;
 *  - the msvcrt printf adapter writes through the OutputSink only, and writes
 *    nothing when the format is rejected;
 *  - trace output goes to stderr only;
 *  - adapter ids of Meta.h, and validation of a reforge_meta blob.
 */

#include <csignal>
#include <cstring>
#include <string>
#include <vector>
#include "xStl/types.h"
#include "xStl/except/trace.h"
#include "xStl/except/assert.h"
#include "reforge/GuestState.h"
#include "reforge/Meta.h"
#include "reforge/rt/Adapter.h"
#include "reforge/rt/AdapterRegistry.h"
#include "reforge/rt/GuestFault.h"
#include "reforge/rt/GuestMemory.h"
#include "reforge/rt/Program.h"
#include "reforge/rt/Runtime.h"
#include "reforge/rt/adapters/Kernel32ExitProcess.h"
#include "reforge/rt/adapters/MsvcrtPrintf.h"
#include "tests.h"

using namespace reforge;
using namespace reforge::rt;

namespace {

const uint64 TEST_BASE = 0x140000000ull;
const uint64 TEST_REGION = 0x2000;

/* Output sink that records what would go to stdout and stderr. */
class CaptureSink : public OutputSink
{
public:
    std::string out;
    std::string err;

    virtual void writeStdout(const void* data, uint64 length)
    {
        out.append(static_cast<const char*>(data), static_cast<std::size_t>(length));
    }

    virtual void writeStderr(const void* data, uint64 length)
    {
        err.append(static_cast<const char*>(data), static_cast<std::size_t>(length));
    }
};

/* Writes a NUL-terminated string at guest address va. */
void putString(GuestMemory& memory, uint64 va, const std::string& text)
{
    memory.write(va, text.c_str(), text.size() + 1);
}

/* Formats fmt with the given (already placed) argument VAs. */
std::string format(GuestMemory& memory, uint64 fmtVA, std::vector<uint64> args)
{
    // The number of real arguments is passed, so a missing one is detected.
    return formatPrintf(memory, fmtVA, args.data(), args.size(), 0x1000);
}

/* True when fn throws GuestFault whose message contains needle. */
template <typename F>
bool faultContains(F fn, const std::string& needle)
{
    try
    {
        fn();
    }
    catch (const GuestFault& e)
    {
        return std::string(e.what()).find(needle) != std::string::npos;
    }
    return false;
}

/* Little-endian stores into a meta blob. */
void put32(std::vector<uint8>& b, uint64 off, uint32 v)
{
    for (int i = 0; i < 4; i++)
        b[static_cast<std::size_t>(off + i)] = static_cast<uint8>(v >> (8 * i));
}

void put64(std::vector<uint8>& b, uint64 off, uint64 v)
{
    for (int i = 0; i < 8; i++)
        b[static_cast<std::size_t>(off + i)] = static_cast<uint8>(v >> (8 * i));
}

/*
 * Builds a valid reforge_meta blob: image of 0x100 bytes at guestBase, two
 * blocks, one import (msvcrt.dll!printf), strings after the tables.
 */
std::vector<uint8> buildMeta(uint64 base)
{
    const uint64 imageOffset = REFORGE_META_HEADER_SIZE;
    const uint64 imageSize = 0x100;
    const uint64 blockOffset = imageOffset + imageSize;
    const uint64 importOffset = blockOffset + 2 * REFORGE_BLOCK_ENTRY_SIZE;
    const uint64 stringsOffset = importOffset + REFORGE_IMPORT_ENTRY_SIZE;
    const std::string strings = std::string("msvcrt.dll") + '\0' + "printf" + '\0';
    const uint64 total = stringsOffset + strings.size();

    std::vector<uint8> b(static_cast<std::size_t>(total), 0);
    put32(b, 0x00, REFORGE_META_MAGIC);
    put32(b, 0x04, REFORGE_META_VERSION);
    put64(b, 0x08, total);
    put64(b, 0x10, base);
    put64(b, 0x18, imageSize);
    put64(b, 0x20, base + 0x10);                 // entryVA
    put64(b, 0x28, 0x200000);                    // regionSize (2 MiB)
    put64(b, 0x30, base + 0x1000);               // thunkVA
    put64(b, 0x38, base + 0x200000);             // stackTopVA
    put64(b, 0x40, imageOffset);
    put64(b, 0x48, 2);                           // blockCount
    put64(b, 0x50, blockOffset);
    put64(b, 0x58, 1);                           // importCount
    put64(b, 0x60, importOffset);
    put64(b, 0x68, stringsOffset);
    put64(b, 0x70, strings.size());
    // Image: the IAT slot at image offset 0x20 holds the thunk VA.
    put64(b, imageOffset + 0x20, base + 0x1000);
    // Blocks: entry point at 0x10 (host 0), a return site at 0x40 (host 0x20).
    put64(b, blockOffset + 0, base + 0x10);
    put64(b, blockOffset + 8, 0);
    put64(b, blockOffset + 16, base + 0x40);
    put64(b, blockOffset + 24, 0x20);
    // Import: adapter, module name at 0, symbol name at 11, IAT VA.
    put32(b, importOffset + 0, ADAPTER_MSVCRT_PRINTF);
    put32(b, importOffset + 4, 0);
    put32(b, importOffset + 8, 11);
    put64(b, importOffset + 16, base + 0x20);
    std::memcpy(&b[static_cast<std::size_t>(stringsOffset)], strings.data(), strings.size());
    return b;
}

} // anonymous namespace

class cTestRuntimeAdapters : public cTestObject
{
public:
    // Literal text is copied, %% becomes %
    void test_printfLiteral()
    {
        GuestMemory mem(TEST_BASE, TEST_REGION);
        putString(mem, TEST_BASE + 0x100, "hello-world\n");
        putString(mem, TEST_BASE + 0x200, "100%% done");
        TESTS_ASSERT_EQUAL(format(mem, TEST_BASE + 0x100, {}), std::string("hello-world\n"));
        TESTS_ASSERT_EQUAL(format(mem, TEST_BASE + 0x200, {}), std::string("100% done"));
    }

    // %s takes guest string pointers in argument order
    void test_printfStrings()
    {
        GuestMemory mem(TEST_BASE, TEST_REGION);
        putString(mem, TEST_BASE + 0x100, "[%s|%s|%s]");
        putString(mem, TEST_BASE + 0x200, "one");
        putString(mem, TEST_BASE + 0x300, "");
        putString(mem, TEST_BASE + 0x400, "three");
        TESTS_ASSERT_EQUAL(format(mem, TEST_BASE + 0x100,
                                  {TEST_BASE + 0x200, TEST_BASE + 0x300, TEST_BASE + 0x400}),
                           std::string("[one||three]"));
    }

    // Any other conversion is rejected and names the conversion
    void test_printfRejectsOtherConversions()
    {
        GuestMemory mem(TEST_BASE, TEST_REGION);
        putString(mem, TEST_BASE + 0x100, "value %d");
        putString(mem, TEST_BASE + 0x200, "width %5s");
        putString(mem, TEST_BASE + 0x300, "tail %");
        TESTS_ASSERT(faultContains([&]() { format(mem, TEST_BASE + 0x100, {1}); },
                                   "unsupported printf conversion '%d'"));
        TESTS_ASSERT(faultContains([&]() { format(mem, TEST_BASE + 0x200, {}); },
                                   "unsupported printf conversion '%5'"));
        TESTS_ASSERT(faultContains([&]() { format(mem, TEST_BASE + 0x300, {}); },
                                   "incomplete printf conversion"));
    }

    // %s with no argument left is an error
    void test_printfMissingArgument()
    {
        GuestMemory mem(TEST_BASE, TEST_REGION);
        putString(mem, TEST_BASE + 0x100, "%s");
        TESTS_ASSERT(faultContains([&]() { format(mem, TEST_BASE + 0x100, {}); },
                                   "without an argument"));
    }

    // Pointers outside guest memory are rejected, including NULL
    void test_printfRejectsOutOfRangePointers()
    {
        GuestMemory mem(TEST_BASE, TEST_REGION);
        putString(mem, TEST_BASE + 0x100, "%s");
        TESTS_ASSERT(faultContains([&]() { format(mem, TEST_BASE + 0x100, {0}); },
                                   "outside guest memory"));
        TESTS_ASSERT(faultContains([&]() { format(mem, TEST_BASE + 0x100, {TEST_BASE - 1}); },
                                   "outside guest memory"));
        TESTS_ASSERT(faultContains([&]() { format(mem, TEST_BASE + 0x100, {TEST_BASE + TEST_REGION}); },
                                   "outside guest memory"));
        // A format pointer outside memory is rejected as well
        TESTS_ASSERT(faultContains([&]() { format(mem, TEST_BASE + TEST_REGION + 4, {}); },
                                   "outside guest memory"));
    }

    // A string without a NUL inside guest memory, or longer than the limit, is rejected
    void test_unterminatedString()
    {
        GuestMemory mem(TEST_BASE, TEST_REGION);
        // 16 non-NUL bytes ending exactly at the end of the region
        for (uint64 i = 0; i < 16; i++)
            mem.write(TEST_BASE + TEST_REGION - 16 + i, "a", 1);
        TESTS_ASSERT(faultContains([&]() { mem.readCString(TEST_BASE + TEST_REGION - 16, 4096); },
                                   "unterminated guest string"));

        // The NUL exists but is beyond maxLength
        putString(mem, TEST_BASE + 0x100, "0123456789");
        TESTS_ASSERT(faultContains([&]() { mem.readCString(TEST_BASE + 0x100, 5); },
                                   "no NUL within 5 bytes"));
        TESTS_ASSERT_EQUAL(mem.readCString(TEST_BASE + 0x100, 10), std::string("0123456789"));
    }

    // Forbidden ranges (the thunk area) cannot be read or written as data
    void test_forbiddenRange()
    {
        GuestMemory mem(TEST_BASE, TEST_REGION);
        mem.forbid(TEST_BASE + 0x1000, 16);
        TESTS_ASSERT(mem.isAccessible(TEST_BASE + 0xff0, 16));
        TESTS_ASSERT(!mem.isAccessible(TEST_BASE + 0xff8, 16));
        TESTS_ASSERT(!mem.isAccessible(TEST_BASE + 0x1000, 1));
        TESTS_ASSERT(mem.isAccessible(TEST_BASE + 0x1010, 1));
        uint8 buffer[4];
        TESTS_ASSERT(faultContains([&]() { mem.read(TEST_BASE + 0x1004, buffer, 4); },
                                   "outside guest memory"));
    }

    // The msvcrt adapter reads a0..a7 and writes only to the standard output sink
    void test_printfAdapter()
    {
        GuestMemory mem(TEST_BASE, TEST_REGION);
        putString(mem, TEST_BASE + 0x100, "hello-world\n");
        putString(mem, TEST_BASE + 0x200, "%s: %%\n");
        putString(mem, TEST_BASE + 0x300, "name");

        ProgramImport import;
        import.module = "msvcrt.dll";
        import.symbol = "printf";
        import.adapter = ADAPTER_MSVCRT_PRINTF;
        import.iatVA = 0;
        import.thunkVA = 0;

        GuestState state;
        std::memset(&state, 0, sizeof(state));
        CaptureSink sink;

        AdapterContext first(state, mem, sink, false, import, TEST_BASE + 0x10);
        state.x[10] = TEST_BASE + 0x100;
        msvcrtPrintfAdapter(first);
        TESTS_ASSERT_EQUAL(sink.out, std::string("hello-world\n"));
        TESTS_ASSERT_EQUAL(state.x[10], 12u);
        TESTS_ASSERT(sink.err.empty());

        AdapterContext second(state, mem, sink, false, import, TEST_BASE + 0x10);
        state.x[10] = TEST_BASE + 0x200;
        state.x[11] = TEST_BASE + 0x300;
        msvcrtPrintfAdapter(second);
        TESTS_ASSERT_EQUAL(sink.out, std::string("hello-world\nname: %\n"));
        TESTS_ASSERT(sink.err.empty());
    }

    // A rejected format writes nothing, not even the text before the conversion
    void test_printfAdapterNoPartialOutput()
    {
        GuestMemory mem(TEST_BASE, TEST_REGION);
        putString(mem, TEST_BASE + 0x100, "partial %d");
        ProgramImport import;
        import.module = "msvcrt.dll";
        import.symbol = "printf";
        import.adapter = ADAPTER_MSVCRT_PRINTF;
        import.iatVA = 0;
        import.thunkVA = 0;
        GuestState state;
        std::memset(&state, 0, sizeof(state));
        state.x[10] = TEST_BASE + 0x100;
        CaptureSink sink;
        AdapterContext context(state, mem, sink, false, import, TEST_BASE + 0x10);
        TESTS_ASSERT(faultContains([&]() { msvcrtPrintfAdapter(context); },
                                   "unsupported printf conversion"));
        TESTS_ASSERT(sink.out.empty());
    }

    // ExitProcess terminates with the exit code; the trace goes to stderr only
    void test_exitProcessAndTrace()
    {
        GuestMemory mem(TEST_BASE, TEST_REGION);
        ProgramImport import;
        import.module = "kernel32.dll";
        import.symbol = "ExitProcess";
        import.adapter = ADAPTER_KERNEL32_EXITPROCESS;
        import.iatVA = 0;
        import.thunkVA = 0;
        GuestState state;
        std::memset(&state, 0, sizeof(state));
        state.x[10] = 0x10F;

        CaptureSink sink;
        AdapterContext context(state, mem, sink, true, import, TEST_BASE + 0x10);
        kernel32ExitProcessAdapter(context);
        TESTS_ASSERT(context.isTerminated());
        TESTS_ASSERT_EQUAL(context.getExitStatus(), 0x10Fu);
        TESTS_ASSERT(sink.out.empty());
        TESTS_ASSERT(sink.err.find("ExitProcess(271)") != std::string::npos);

        // Tracing disabled: nothing is written at all
        CaptureSink quiet;
        AdapterContext silent(state, mem, quiet, false, import, TEST_BASE + 0x10);
        kernel32ExitProcessAdapter(silent);
        TESTS_ASSERT(quiet.err.empty());
        TESTS_ASSERT(quiet.out.empty());
    }

    // Adapter ids are looked up by module (case-insensitive) and symbol
    void test_adapterIds()
    {
        TESTS_ASSERT_EQUAL(findAdapterId("msvcrt.dll", "printf"), ADAPTER_MSVCRT_PRINTF);
        TESTS_ASSERT_EQUAL(findAdapterId("MSVCRT.DLL", "printf"), ADAPTER_MSVCRT_PRINTF);
        TESTS_ASSERT_EQUAL(findAdapterId("kernel32.dll", "ExitProcess"),
                           ADAPTER_KERNEL32_EXITPROCESS);
        TESTS_ASSERT_EQUAL(findAdapterId("msvcrt.dll", "Printf"), ADAPTER_NONE);
        TESTS_ASSERT_EQUAL(findAdapterId("msvcrt.dll", "puts"), ADAPTER_NONE);
        TESTS_ASSERT_EQUAL(findAdapterId("user32.dll", "printf"), ADAPTER_NONE);
        TESTS_ASSERT(lookupAdapter(ADAPTER_MSVCRT_PRINTF) != nullptr);
        TESTS_ASSERT(lookupAdapter(ADAPTER_KERNEL32_EXITPROCESS) != nullptr);
        TESTS_ASSERT(lookupAdapter(ADAPTER_NONE) == nullptr);
    }

    // A well-formed blob parses, and the lookups use the documented tables
    void test_metaParse()
    {
        std::vector<uint8> blob = buildMeta(TEST_BASE);
        Program p = Program::parse(blob.data(), blob.size());
        TESTS_ASSERT_EQUAL(p.entryVA, TEST_BASE + 0x10);
        TESTS_ASSERT_EQUAL(p.imports.size(), 1u);
        TESTS_ASSERT_EQUAL(p.imports[0].module, std::string("msvcrt.dll"));
        TESTS_ASSERT_EQUAL(p.imports[0].symbol, std::string("printf"));
        TESTS_ASSERT_EQUAL(p.imports[0].thunkVA, TEST_BASE + 0x1000);
        uint64 host = 0;
        TESTS_ASSERT(p.lookupBlock(TEST_BASE + 0x40, host));
        TESTS_ASSERT_EQUAL(host, 0x20u);
        TESTS_ASSERT(!p.lookupBlock(TEST_BASE + 0x44, host));
        uint64 index = 7;
        TESTS_ASSERT(p.lookupThunk(TEST_BASE + 0x1000, index));
        TESTS_ASSERT_EQUAL(index, 0u);
        TESTS_ASSERT(!p.lookupThunk(TEST_BASE + 0x1008, index));
        TESTS_ASSERT(!p.lookupThunk(TEST_BASE + 0x1010, index));
    }

    // Blobs with a wrong magic, size, block order or entry are rejected
    void test_metaRejectsBadBlobs()
    {
        std::vector<uint8> good = buildMeta(TEST_BASE);

        std::vector<uint8> badMagic = good;
        badMagic[0] ^= 0xff;
        TESTS_ASSERT(faultContains([&]() { Program::parse(badMagic.data(), badMagic.size()); },
                                   "bad magic"));

        TESTS_ASSERT(faultContains([&]() { Program::parse(good.data(), good.size() - 1); },
                                   "total size"));

        std::vector<uint8> unsorted = good;
        put64(unsorted, REFORGE_META_HEADER_SIZE + 0x100 + 0, TEST_BASE + 0x40);
        put64(unsorted, REFORGE_META_HEADER_SIZE + 0x100 + 16, TEST_BASE + 0x10);
        TESTS_ASSERT(faultContains([&]() { Program::parse(unsorted.data(), unsorted.size()); },
                                   "strictly ascending"));

        std::vector<uint8> badEntry = good;
        put64(badEntry, 0x20, TEST_BASE + 0x100000);
        TESTS_ASSERT(faultContains([&]() { Program::parse(badEntry.data(), badEntry.size()); },
                                   "entry point"));

        std::vector<uint8> badAdapter = good;
        put32(badAdapter, REFORGE_META_HEADER_SIZE + 0x100 + 2 * 16, 99);
        TESTS_ASSERT(faultContains([&]() { Program::parse(badAdapter.data(), badAdapter.size()); },
                                   "no adapter"));
    }

    // The fault handler's diagnostic: both addresses, the signal name, bounded length.
    void test_faultLine()
    {
        char line[256];
        std::size_t length = rt::formatFaultLine(line, sizeof(line), SIGSEGV,
                                                 0x7f0000001234ull, 0x7f0000000000ull);
        TESTS_ASSERT_EQUAL(std::string(line, length),
                           std::string("reforge-rt: runtime error: segmentation fault at host "
                                       "address 0x7f0000001234 (guest address 0x1234 if this was "
                                       "a translated load or store outside guest memory)\n"));

        length = rt::formatFaultLine(line, sizeof(line), SIGBUS, 0, 0);
        TESTS_ASSERT(std::string(line, length).find("bus error at host address 0x0 "
                                                    "(guest address 0x0 if") != std::string::npos);

        // A small buffer is filled up to capacity - 1 bytes and never overrun.
        char small[8];
        std::memset(small, 'x', sizeof(small));
        length = rt::formatFaultLine(small, sizeof(small), SIGSEGV, 0x1000, 0);
        TESTS_ASSERT_EQUAL(length, sizeof(small) - 1);
        TESTS_ASSERT_EQUAL(std::string(small, length), std::string("reforge"));
        TESTS_ASSERT_EQUAL(small[7], 'x');
    }

    // Perform the test
    virtual void test()
    {
        test_faultLine();
        test_printfLiteral();
        test_printfStrings();
        test_printfRejectsOtherConversions();
        test_printfMissingArgument();
        test_printfRejectsOutOfRangePointers();
        test_unterminatedString();
        test_forbiddenRange();
        test_printfAdapter();
        test_printfAdapterNoPartialOutput();
        test_exitProcessAndTrace();
        test_adapterIds();
        test_metaParse();
        test_metaRejectsBadBlobs();
    }

    // Return the name of the module
    virtual cString getName() { return __FILE__; }
};

// Instance test object
cTestRuntimeAdapters g_globalTestRuntimeAdapters;
