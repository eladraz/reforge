/*
 * PeLoader.cpp
 *
 * Loads a PE32+ RISC-V64 console image into a GuestImage.
 *
 * The file is validated from its bytes first (signatures, machine, magic,
 * subsystem, section table and raw data inside the file, SizeOfImage, entry
 * point). Only then is the header parsed by the pe library (cNtHeader) and the
 * import directory read by cNtDirImport, so that a truncated file is reported
 * as such and not as a library exception.
 */
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>
#include "reforge/Diagnostics.h"
#include "reforge/PeLoader.h"
#include "xStl/types.h"
#include "xStl/data/sarray.h"
#include "xStl/data/string.h"
#include "xStl/data/list.h"
#include "xStl/except/exception.h"
#include "xStl/stream/memoryStream.h"
#include "pe/datastruct.h"
#include "pe/ntheader.h"
#include "pe/ntDirImport.h"

namespace reforge {

namespace {

const uint64 MAX_SIZE_OF_IMAGE = 256ull * 1024 * 1024;
const uint64 DOS_HEADER_SIZE = 0x40;
const uint64 PE_SIGNATURE_SIZE = 4;
const uint64 FILE_HEADER_SIZE = 20;
const uint64 SECTION_HEADER_SIZE = 40;
const uint64 MAX_SECTIONS = 96;
const uint32 SCN_MEM_EXECUTE = 0x20000000;
const uint32 SCN_MEM_READ = 0x40000000;
const uint32 SCN_MEM_WRITE = 0x80000000;

[[noreturn]] void malformed(const std::string& message)
{
    throw ReforgeError(RC_MALFORMED_INPUT, message);
}

std::string hex(uint64 value)
{
    std::ostringstream out;
    out << "0x" << std::hex << value;
    return out.str();
}

uint16 readLe16(const std::vector<uint8>& file, uint64 offset)
{
    return (uint16)(file[offset] | (file[offset + 1] << 8));
}

uint32 readLe32(const std::vector<uint8>& file, uint64 offset)
{
    return (uint32)file[offset] | ((uint32)file[offset + 1] << 8) |
           ((uint32)file[offset + 2] << 16) | ((uint32)file[offset + 3] << 24);
}

uint64 readLe64(const std::vector<uint8>& file, uint64 offset)
{
    return (uint64)readLe32(file, offset) | ((uint64)readLe32(file, offset + 4) << 32);
}

/* Converts a pe-library string (wide when XSTL_UNICODE) to a narrow string. */
std::string toNarrow(const cString& value)
{
    std::string result;
    const character* buffer = value.getBuffer();
    for (uint i = 0; i < value.length(); i++)
        result += (char)buffer[i];
    return result;
}

std::string toNarrow(const character* value)
{
    std::string result;
    for (; value && *value; value++)
        result += (char)*value;
    return result;
}

std::string lowerCase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return value;
}

/* One section header, with the span of the section in the image resolved. */
struct SectionInfo
{
    std::string name;
    Rva rva;
    uint64 virtualSize;
    FileOffset fileOffset;
    uint64 rawSize;
    uint32 characteristics;
    /* max(VirtualSize, SizeOfRawData) when VirtualSize is set, else SizeOfRawData. */
    uint64 span;
};

/* Reads the section table (already known to lie in the file) and checks the RVA ranges. */
std::vector<SectionInfo> readSections(const std::vector<uint8>& file,
                                      uint64 tableOffset,
                                      uint64 count,
                                      uint64 sizeOfImage)
{
    std::vector<SectionInfo> sections;
    for (uint64 i = 0; i < count; i++)
    {
        const uint64 at = tableOffset + i * SECTION_HEADER_SIZE;
        SectionInfo section;
        for (uint64 c = 0; c < 8 && file[at + c]; c++)
            section.name += (char)file[at + c];
        section.virtualSize = readLe32(file, at + 8);
        section.rva = Rva(readLe32(file, at + 12));
        section.rawSize = readLe32(file, at + 16);
        section.fileOffset = FileOffset(readLe32(file, at + 20));
        section.characteristics = readLe32(file, at + 36);
        section.span = section.virtualSize ?
            std::max(section.virtualSize, section.rawSize) : section.rawSize;

        if (section.span > sizeOfImage ||
            section.rva.value > sizeOfImage - section.span)
        {
            malformed("section '" + section.name + "' (RVA " +
                      hex(section.rva.value) + ", size " + hex(section.span) +
                      ") extends past SizeOfImage " + hex(sizeOfImage));
        }
        sections.push_back(section);
    }
    return sections;
}

/* The raw data of a section is copied from the file, so it must be inside the file. */
void checkRawData(const SectionInfo& section, uint64 fileSize)
{
    if (section.rawSize == 0)
        return;
    if (section.fileOffset.value > fileSize ||
        section.rawSize > fileSize - section.fileOffset.value)
    {
        malformed("truncated: section '" + section.name + "' raw data [" +
                  hex(section.fileOffset.value) + ", " +
                  hex(section.fileOffset.value + section.rawSize) +
                  ") is beyond the end of the file (" + hex(fileSize) + " bytes)");
    }
}

/* Checks that no two sections share a part of the image. */
void checkNoOverlap(std::vector<SectionInfo> sections)
{
    std::sort(sections.begin(), sections.end(),
              [](const SectionInfo& a, const SectionInfo& b)
              { return a.rva.value < b.rva.value; });
    for (size_t i = 1; i < sections.size(); i++)
    {
        const SectionInfo& prev = sections[i - 1];
        const SectionInfo& cur = sections[i];
        if (prev.span && cur.span && prev.rva.value + prev.span > cur.rva.value)
            malformed("section '" + prev.name + "' overlaps section '" +
                      cur.name + "' at RVA " + hex(cur.rva.value));
    }
}

/* Offset of a field of the PE32+ optional header, given the header's file offset. */
#define OPTIONAL_FIELD(base, field) ((base) + offsetof(IMAGE_OPTIONAL_HEADER64, field))

} // anonymous namespace

GuestImage loadPeImage(const std::vector<uint8>& file)
{
    const uint64 fileSize = file.size();

    // DOS header and PE signature.
    if (fileSize < DOS_HEADER_SIZE)
        malformed("truncated: file is " + hex(fileSize) +
                  " bytes, smaller than the DOS header");
    if (file[0] != 'M' || file[1] != 'Z')
        malformed("not a PE file: missing MZ signature");

    const uint64 ntOffset = readLe32(file, 0x3c);
    if (ntOffset < DOS_HEADER_SIZE || ntOffset > fileSize ||
        fileSize - ntOffset < PE_SIGNATURE_SIZE + FILE_HEADER_SIZE)
    {
        malformed("truncated: NT headers at " + hex(ntOffset) +
                  " do not fit in the file (" + hex(fileSize) + " bytes)");
    }
    if (readLe32(file, ntOffset) != 0x00004550)
        malformed("not a PE file: missing PE signature at " + hex(ntOffset));

    // File header: the machine is checked first, so a foreign image is named as such.
    const uint64 fileHeader = ntOffset + PE_SIGNATURE_SIZE;
    const uint16 machine = readLe16(file, fileHeader);
    if (machine != IMAGE_FILE_MACHINE_RISCV64)
    {
        malformed("wrong machine " + hex(machine) +
                  " (expected IMAGE_FILE_MACHINE_RISCV64 0x5064)");
    }

    const uint16 sectionCount = readLe16(file, fileHeader + 2);
    const uint16 optionalSize = readLe16(file, fileHeader + 16);
    const uint64 optionalHeader = fileHeader + FILE_HEADER_SIZE;
    if (optionalHeader > fileSize || fileSize - optionalHeader < optionalSize)
        malformed("truncated: optional header does not fit in the file");

    const uint16 magic = readLe16(file, optionalHeader);
    if (magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        malformed("not a PE32+ image: optional header magic " + hex(magic) +
                  " (expected 0x20b)");

    if (optionalSize < sizeof(IMAGE_OPTIONAL_HEADER64))
        malformed("truncated: SizeOfOptionalHeader " + hex(optionalSize) +
                  " is smaller than the PE32+ optional header (" +
                  hex(sizeof(IMAGE_OPTIONAL_HEADER64)) + ")");
    if (sectionCount == 0 || sectionCount > MAX_SECTIONS)
        malformed("bad section count " + std::to_string(sectionCount));
    const uint64 sectionTable = optionalHeader + optionalSize;
    if (sectionTable > fileSize ||
        fileSize - sectionTable < sectionCount * SECTION_HEADER_SIZE)
    {
        malformed("truncated: section table does not fit in the file (" +
                  hex(fileSize) + " bytes)");
    }

    const uint16 subsystem = readLe16(file, OPTIONAL_FIELD(optionalHeader, Subsystem));
    if (subsystem != IMAGE_SUBSYSTEM_WINDOWS_CUI)
        malformed("non-console subsystem " + std::to_string(subsystem) +
                  " (expected IMAGE_SUBSYSTEM_WINDOWS_CUI 3)");

    // Image-level fields, read from the optional header.
    const uint64 sizeOfImage = readLe32(file, OPTIONAL_FIELD(optionalHeader, SizeOfImage));
    if (sizeOfImage == 0 || sizeOfImage > MAX_SIZE_OF_IMAGE)
        malformed("SizeOfImage " + hex(sizeOfImage) + " is out of range (max 256 MiB)");
    const uint64 sectionAlignment = readLe32(file, OPTIONAL_FIELD(optionalHeader, SectionAlignment));
    if (sectionAlignment == 0 || (sectionAlignment & (sectionAlignment - 1)))
        malformed("SectionAlignment " + hex(sectionAlignment) + " is not a power of two");
    const uint64 sizeOfHeaders = readLe32(file, OPTIONAL_FIELD(optionalHeader, SizeOfHeaders));
    if (sizeOfHeaders > sizeOfImage)
        malformed("SizeOfHeaders " + hex(sizeOfHeaders) + " exceeds SizeOfImage");
    const uint64 imageBase = readLe64(file, OPTIONAL_FIELD(optionalHeader, ImageBase));
    if (imageBase == 0 || imageBase > UINT64_MAX - sizeOfImage)
        malformed("ImageBase " + hex(imageBase) + " is invalid");
    const uint64 entryRva = readLe32(file, OPTIONAL_FIELD(optionalHeader, AddressOfEntryPoint));

    // Sections: RVA ranges, raw data inside the file, permissions, no overlap.
    const std::vector<SectionInfo> raw = readSections(file, sectionTable,
                                                     sectionCount, sizeOfImage);
    std::vector<GuestSection> sections;
    for (const SectionInfo& section : raw)
    {
        checkRawData(section, fileSize);
        const bool readable = (section.characteristics & SCN_MEM_READ) != 0;
        const bool writable = (section.characteristics & SCN_MEM_WRITE) != 0;
        const bool executable = (section.characteristics & SCN_MEM_EXECUTE) != 0;
        if (writable && executable)
            malformed("section '" + section.name + "' is both writable and executable");

        GuestSection out;
        out.name = section.name;
        out.rva = section.rva.value;
        out.virtualSize = section.virtualSize;
        out.fileOffset = section.fileOffset.value;
        out.rawSize = section.rawSize;
        out.readable = readable;
        out.writable = writable;
        out.executable = executable;
        sections.push_back(out);
    }
    checkNoOverlap(raw);

    // Entry point: an RVA inside an executable section.
    bool entryOk = false;
    for (const SectionInfo& section : raw)
    {
        if ((section.characteristics & SCN_MEM_EXECUTE) &&
            entryRva >= section.rva.value &&
            entryRva - section.rva.value < section.span)
        {
            entryOk = true;
            break;
        }
    }
    if (!entryOk)
        malformed("entry point RVA " + hex(entryRva) +
                  " is not inside an executable section");

    // Base relocation directory: bounds only, relocations are not applied.
    const uint64 relocsField = OPTIONAL_FIELD(optionalHeader, DataDirectory) +
                               IMAGE_DIRECTORY_ENTRY_BASERELOC * sizeof(IMAGE_DATA_DIRECTORY);
    const uint64 relocsRva = readLe32(file, relocsField);
    const uint64 relocsSize = readLe32(file, relocsField + 4);
    if (relocsSize != 0 &&
        (relocsRva > sizeOfImage || relocsSize > sizeOfImage - relocsRva))
    {
        malformed("base relocation directory (RVA " + hex(relocsRva) +
                  ", size " + hex(relocsSize) + ") is outside the image");
    }

    // Import directory: bounds only (the pe library walks the descriptors).
    const uint64 importsField = OPTIONAL_FIELD(optionalHeader, DataDirectory) +
                                IMAGE_DIRECTORY_ENTRY_IMPORT * sizeof(IMAGE_DATA_DIRECTORY);
    const uint64 importsRva = readLe32(file, importsField);
    const uint64 importsSize = readLe32(file, importsField + 4);
    if (importsSize != 0 &&
        (importsRva > sizeOfImage || importsSize > sizeOfImage - importsRva))
    {
        malformed("import directory (RVA " + hex(importsRva) + ", size " +
                  hex(importsSize) + ") is outside the image");
    }

    // The pe library parses the headers and the import directory. The file has
    // been checked above, so a failure here is a malformed import table.
    cBuffer buffer((uint)fileSize);
    std::copy(file.begin(), file.end(), buffer.getBuffer());
    cMemoryStream stream(buffer);
    cList<cNtDirImport::Module> modules;
    try
    {
        stream.seek(ntOffset, basicInput::IO_SEEK_SET);
        cNtHeader header(stream);
        if (!header.is64bit())
            malformed("not a PE32+ image: the pe library reads it as PE32");
        cNtDirImport imports(header);
        modules = imports.getModules();
    }
    catch (cException& e)
    {
        cString where;
        e.whereExceptionOccurred(where);
        std::string detail = toNarrow(e.getMessage());
        if (detail.empty())
            detail = "pe library exception " + std::to_string((unsigned)e.getID());
        malformed("malformed import table or headers: " + detail + " (" +
                  toNarrow(where) + ")");
    }

    // Imports, with the IAT slot of every entry as a VA.
    std::vector<GuestImport> imports;
    for (cList<cNtDirImport::Module>::iterator module = modules.begin();
         module != modules.end(); ++module)
    {
        const std::string moduleName = lowerCase(toNarrow((*module).name));
        for (cList<cNtDirImport::Entry>::iterator entry = (*module).entries.begin();
             entry != (*module).entries.end(); ++entry)
        {
            GuestImport item;
            item.module = moduleName;
            if ((*entry).byOrdinal)
                item.symbol = "#" + std::to_string((unsigned)(*entry).ordinal);
            else
                item.symbol = toNarrow((*entry).name);
            if ((*entry).iatRva + 8 > sizeOfImage)
                malformed("IAT slot of " + moduleName + "!" + item.symbol +
                          " at RVA " + hex((*entry).iatRva) + " is outside the image");
            item.iatVA = imageBase + (*entry).iatRva;
            imports.push_back(item);
        }
    }

    // Lay the image out by RVA: the headers, then each section's initialized bytes.
    GuestImage result;
    result.base = imageBase;
    result.sizeOfImage = sizeOfImage;
    result.entryVA = imageBase + entryRva;
    result.image.assign(sizeOfImage, 0);
    const uint64 headerCopy = std::min<uint64>(std::min(sizeOfHeaders, fileSize), sizeOfImage);
    std::copy(file.begin(), file.begin() + headerCopy, result.image.begin());
    for (const SectionInfo& section : raw)
    {
        uint64 copy = section.rawSize;
        if (section.virtualSize && section.virtualSize < copy)
            copy = section.virtualSize;
        if (copy == 0)
            continue;
        std::copy(file.begin() + section.fileOffset.value,
                  file.begin() + section.fileOffset.value + copy,
                  result.image.begin() + section.rva.value);
    }
    result.sections = sections;
    result.imports = imports;
    return result;
}

GuestImage loadPeFile(const std::string& path)
{
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input)
        malformed("cannot read input '" + path + "'");
    std::vector<uint8> file((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
    if (input.bad())
        malformed("cannot read input '" + path + "'");
    return loadPeImage(file);
}

} // namespace reforge
