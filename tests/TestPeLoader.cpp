/*
 * TestPeLoader.cpp
 *
 * Tests of the PE loader (reforge::loadPeImage / loadPeFile) on the fixtures
 * written by scripts/build_fixtures.sh. The fixture directory is taken from
 * the REFORGE_FIXTURES environment variable, else from the CMake cache
 * (REFORGE_FIXTURES_DEFAULT). Negative cases mutate the fixture bytes in
 * memory, so each one changes exactly one header field.
 */
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>
#include "tests.h"
#include "reforge/Diagnostics.h"
#include "reforge/GuestImage.h"
#include "reforge/PeLoader.h"
#include "pe/datastruct.h"

using namespace reforge;

namespace {

void failTest(const std::string& message)
{
    std::cerr << "test_reforge: " << message << std::endl;
    TESTS_ASSERT(false);
}

#define EXPECT(condition)                                                    \
    do {                                                                     \
        if (!(condition))                                                    \
            failTest(std::string(__FILE__) + ":" + std::to_string(__LINE__) + \
                     ": expected " #condition);                              \
    } while (0)

std::string fixturePath(const std::string& relative)
{
    const char* dir = getenv("REFORGE_FIXTURES");
    std::string root = dir ? dir : REFORGE_FIXTURE_DIR;
    return root + "/" + relative;
}

std::vector<uint8> readFile(const std::string& path)
{
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input)
        failTest("missing fixture " + path +
                 " (run scripts/build_fixtures.sh, or set REFORGE_FIXTURES)");
    return std::vector<uint8>((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
}

uint32 getLe32(const std::vector<uint8>& bytes, uint64 offset)
{
    return (uint32)bytes[offset] | ((uint32)bytes[offset + 1] << 8) |
           ((uint32)bytes[offset + 2] << 16) | ((uint32)bytes[offset + 3] << 24);
}

void setLe16(std::vector<uint8>& bytes, uint64 offset, uint16 value)
{
    bytes[offset] = (uint8)value;
    bytes[offset + 1] = (uint8)(value >> 8);
}

void setLe32(std::vector<uint8>& bytes, uint64 offset, uint32 value)
{
    for (int i = 0; i < 4; i++)
        bytes[offset + i] = (uint8)(value >> (8 * i));
}

/* File offset of the optional header (PE32+) of a fixture. */
uint64 optionalHeaderOffset(const std::vector<uint8>& bytes)
{
    return getLe32(bytes, 0x3c) + 4 + sizeof(IMAGE_FILE_HEADER);
}

const GuestSection& findSection(const GuestImage& image, const std::string& name)
{
    for (size_t i = 0; i < image.sections.size(); i++)
        if (image.sections[i].name == name)
            return image.sections[i];
    failTest("section " + name + " not found");
    return image.sections[0];
}

bool hasImport(const GuestImage& image, const std::string& module,
               const std::string& symbol)
{
    for (size_t i = 0; i < image.imports.size(); i++)
        if (image.imports[i].module == module && image.imports[i].symbol == symbol)
            return true;
    return false;
}

bool containsBytes(const std::vector<uint8>& haystack, const std::string& needle)
{
    if (needle.empty() || haystack.size() < needle.size())
        return false;
    for (size_t i = 0; i + needle.size() <= haystack.size(); i++)
        if (std::equal(needle.begin(), needle.end(), haystack.begin() + i))
            return true;
    return false;
}

/*
 * Loads the bytes and requires the loader to reject them with
 * RC_MALFORMED_INPUT and a message containing 'needle'.
 */
void expectMalformed(const std::vector<uint8>& bytes, const std::string& needle,
                     const std::string& what)
{
    try
    {
        loadPeImage(bytes);
    }
    catch (const ReforgeError& e)
    {
        std::string message = e.what();
        if (e.getExitCode() != RC_MALFORMED_INPUT)
            failTest(what + ": exit code " + std::to_string((int)e.getExitCode()) +
                     ", expected 3");
        if (message.find(needle) == std::string::npos)
            failTest(what + ": message '" + message + "' does not contain '" +
                     needle + "'");
        return;
    }
    failTest(what + ": the loader accepted the malformed image");
}

class TestPeLoader : public cTestObject
{
public:
    void test()
    {
        try
        {
            testHelloLoads();
            testComputeLoads();
            testBadFixturesAreWellFormedPe();
            testMissingFile();
            testWrongMachine();
            testTruncated();
            testNotPe32Plus();
            testBadEntryRva();
            testNonConsoleSubsystem();
            testOverlongSizeOfImage();
            testMalformedImportTable();
        }
        catch (const std::exception& e)
        {
            // Report loader errors by name instead of as an unknown exception.
            failTest(std::string("unexpected error: ") + e.what());
        }
    }

    cString getName()
    {
        return XSTL_STRING("reforge PE loader");
    }

private:
    static void testHelloLoads()
    {
        const std::string path = fixturePath("hello-riscv64-windows.exe");
        const GuestImage image = loadPeFile(path);
        const std::vector<uint8> file = readFile(path);

        EXPECT(image.base == 0x140000000ull);
        EXPECT(image.sizeOfImage == image.image.size());
        EXPECT(image.image.size() % 0x1000 == 0);
        EXPECT(image.image[0] == 'M' && image.image[1] == 'Z');

        // Sections: .text RX, .rdata R, .idata RW at the fixed RVAs. The hello
        // fixture has no .data (it has no writable data of its own).
        EXPECT(image.sections.size() == 3);
        const GuestSection& text = findSection(image, ".text");
        EXPECT(text.rva == 0x1000 && text.executable && text.readable && !text.writable);
        const GuestSection& rdata = findSection(image, ".rdata");
        EXPECT(rdata.rva == 0x2000 && rdata.readable && !rdata.writable && !rdata.executable);
        const GuestSection& idata = findSection(image, ".idata");
        EXPECT(idata.rva == 0x4000 && idata.readable && idata.writable && !idata.executable);
        for (size_t i = 0; i < image.sections.size(); i++)
            EXPECT(!(image.sections[i].writable && image.sections[i].executable));

        // The entry point is a VA inside .text.
        EXPECT(image.entryVA >= image.base + text.rva);
        EXPECT(image.entryVA < image.base + text.rva + text.virtualSize);

        // Layout by RVA: the section bytes are at their RVA in the image.
        for (size_t i = 0; i < image.sections.size(); i++)
        {
            const GuestSection& s = image.sections[i];
            uint64 copy = std::min(s.rawSize, s.virtualSize ? s.virtualSize : s.rawSize);
            for (uint64 b = 0; b < copy; b++)
                EXPECT(image.image[s.rva + b] == file[s.fileOffset + b]);
        }

        // The message is in .rdata, and the imports are msvcrt!printf and
        // kernel32!ExitProcess, with IAT slots inside .idata.
        EXPECT(containsBytes(image.image, std::string("hello-world\n\0", 13)));
        EXPECT(image.imports.size() == 2);
        EXPECT(hasImport(image, "msvcrt.dll", "printf"));
        EXPECT(hasImport(image, "kernel32.dll", "ExitProcess"));
        for (size_t i = 0; i < image.imports.size(); i++)
        {
            const GuestImport& imp = image.imports[i];
            EXPECT(imp.iatVA >= image.base + idata.rva);
            EXPECT(imp.iatVA + 8 <= image.base + idata.rva + idata.virtualSize);
            EXPECT((imp.iatVA - image.base) % 8 == 0);
        }
    }

    static void testComputeLoads()
    {
        const GuestImage image = loadPeFile(fixturePath("compute-riscv64-windows.exe"));
        const std::vector<uint8>& bytes = image.image;
        EXPECT(image.sections.size() == 4);
        EXPECT(findSection(image, ".text").executable);
        // .data is the writable, non-executable section.
        const GuestSection& data = findSection(image, ".data");
        EXPECT(data.readable && data.writable && !data.executable);
        EXPECT(containsBytes(bytes, std::string("sum-ok\n\0", 8)));
        EXPECT(containsBytes(bytes, std::string("sum-bad\n\0", 9)));
        EXPECT(hasImport(image, "msvcrt.dll", "printf"));
        EXPECT(hasImport(image, "kernel32.dll", "ExitProcess"));
        EXPECT(image.entryVA != image.base);
    }

    static void testBadFixturesAreWellFormedPe()
    {
        // The bad fixtures are malformed guest code or imports, not malformed PE:
        // the loader accepts them, and the later stages reject them.
        const GuestImage ecall = loadPeFile(fixturePath("bad/ecall-riscv64-windows.exe"));
        EXPECT(hasImport(ecall, "kernel32.dll", "ExitProcess"));
        const GuestImage unresolved =
            loadPeFile(fixturePath("bad/unresolved_import-riscv64-windows.exe"));
        EXPECT(hasImport(unresolved, "kernel32.dll", "Sleep"));
    }

    static void testMissingFile()
    {
        try
        {
            loadPeFile(fixturePath("no-such-file.exe"));
        }
        catch (const ReforgeError& e)
        {
            EXPECT(e.getExitCode() == RC_MALFORMED_INPUT);
            EXPECT(std::string(e.what()).find("cannot read input") != std::string::npos);
            return;
        }
        failTest("loadPeFile accepted a missing file");
    }

    static void testWrongMachine()
    {
        std::vector<uint8> bytes = readFile(fixturePath("hello-riscv64-windows.exe"));
        setLe16(bytes, getLe32(bytes, 0x3c) + 4, IMAGE_FILE_MACHINE_AMD64);
        expectMalformed(bytes, "wrong machine", "wrong machine");
    }

    static void testTruncated()
    {
        const std::vector<uint8> full = readFile(fixturePath("hello-riscv64-windows.exe"));

        std::vector<uint8> bytes(full.begin(), full.begin() + 700);
        expectMalformed(bytes, "truncated", "truncated at 700 bytes");

        bytes.assign(full.begin(), full.begin() + 0x20);
        expectMalformed(bytes, "truncated", "truncated inside the DOS header");

        bytes.assign(full.begin(), full.begin() + 0x100);
        expectMalformed(bytes, "truncated", "truncated inside the NT headers");

        bytes.assign(full.begin(), full.begin() + full.size() - 1);
        expectMalformed(bytes, "truncated", "last byte missing");
    }

    static void testNotPe32Plus()
    {
        std::vector<uint8> bytes = readFile(fixturePath("hello-riscv64-windows.exe"));
        setLe16(bytes, optionalHeaderOffset(bytes), IMAGE_NT_OPTIONAL_HDR32_MAGIC);
        expectMalformed(bytes, "PE32+", "PE32 magic");
    }

    static void testBadEntryRva()
    {
        const std::vector<uint8> original = readFile(fixturePath("hello-riscv64-windows.exe"));
        const uint64 entryField = optionalHeaderOffset(original) +
                                  offsetof(IMAGE_OPTIONAL_HEADER64, AddressOfEntryPoint);

        // In .rdata (not executable), in the headers, and past the image.
        const uint32 entries[] = { 0x2000, 0, 0x7fffffff };
        for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++)
        {
            std::vector<uint8> bytes = original;
            setLe32(bytes, entryField, entries[i]);
            expectMalformed(bytes, "entry point", "bad entry RVA");
        }
    }

    static void testNonConsoleSubsystem()
    {
        std::vector<uint8> bytes = readFile(fixturePath("hello-riscv64-windows.exe"));
        const uint64 subsystemField = optionalHeaderOffset(bytes) +
                                      offsetof(IMAGE_OPTIONAL_HEADER64, Subsystem);
        setLe16(bytes, subsystemField, IMAGE_SUBSYSTEM_WINDOWS_GUI);
        expectMalformed(bytes, "non-console subsystem", "GUI subsystem");
    }

    // Every mutation damages one field of the import table; each must be
    // rejected as a malformed image (exit 3), never crash, hang or be accepted.
    static void testMalformedImportTable()
    {
        const std::vector<uint8> original = readFile(fixturePath("hello-riscv64-windows.exe"));
        const uint64 dir = optionalHeaderOffset(original) +
                           offsetof(IMAGE_OPTIONAL_HEADER64, DataDirectory) +
                           IMAGE_DIRECTORY_ENTRY_IMPORT * sizeof(IMAGE_DATA_DIRECTORY);
        const uint32 importRva = getLe32(original, dir);
        // .idata is the last section of the fixture; find its file offset.
        const uint64 sections = optionalHeaderOffset(original) + 0xf0;
        uint64 idataFile = 0;
        uint32 idataRva = 0;
        for (int i = 0; i < 3; i++)
        {
            const uint64 at = sections + 40 * i;
            if (original[at + 1] == 'i')   // ".idata"
            {
                idataRva = getLe32(original, at + 12);
                idataFile = getLe32(original, at + 20);
            }
        }
        if (idataFile == 0)
            failTest("no .idata section in the hello fixture");
        const uint64 descriptor = idataFile + (importRva - idataRva);

        std::vector<uint8> bytes = original;
        setLe32(bytes, dir, 0x7ffff000);
        expectMalformed(bytes, "import directory", "import directory RVA outside the image");

        bytes = original;
        setLe32(bytes, dir + 4, 0x10000000);
        expectMalformed(bytes, "import directory", "import directory size outside the image");

        const uint32 fields[] = { 0, 12, 16 };   // ILT RVA, name RVA, IAT RVA
        const char* needles[] = { "import table", "import table", "IAT slot" };
        for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
        {
            bytes = original;
            setLe32(bytes, descriptor + fields[i], 0x7ffffff0);
            expectMalformed(bytes, needles[i], "import descriptor field outside the image");
        }

        bytes = original;   // an IAT slot RVA of zero (headers), thunk data of garbage
        setLe32(bytes, descriptor + 16, 0);
        expectMalformed(bytes, "import table", "IAT RVA 0");

        bytes = original;   // the descriptor array is never terminated
        for (uint64 i = 20; i < 40; i++)
            bytes[descriptor + i] = 1;
        expectMalformed(bytes, "import table", "unterminated descriptor array");

        bytes = original;   // optional header too small to hold the directories
        setLe16(bytes, getLe32(bytes, 0x3c) + 4 + 16, 0x40);
        expectMalformed(bytes, "SizeOfOptionalHeader", "tiny SizeOfOptionalHeader");
    }

    static void testOverlongSizeOfImage()
    {
        std::vector<uint8> bytes = readFile(fixturePath("hello-riscv64-windows.exe"));
        const uint64 sizeField = optionalHeaderOffset(bytes) +
                                 offsetof(IMAGE_OPTIONAL_HEADER64, SizeOfImage);
        setLe32(bytes, sizeField, 0x20000000);  // 512 MiB
        expectMalformed(bytes, "SizeOfImage", "SizeOfImage above 256 MiB");
    }
};

TestPeLoader g_testPeLoader;

} // anonymous namespace
