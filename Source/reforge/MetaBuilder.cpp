/*
 * MetaBuilder.cpp
 *
 * See MetaBuilder.h and Meta.h (the layout). The blob is appended section by
 * section; every table offset is 8-byte aligned.
 */
#include <cstring>
#include <map>
#include <sstream>
#include "reforge/Diagnostics.h"
#include "reforge/MetaBuilder.h"

namespace reforge {

namespace {

uint64 alignUp(uint64 value, uint64 alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

void put(std::vector<uint8>& blob, uint64 offset, const void* data, std::size_t size)
{
    std::memcpy(&blob[static_cast<std::size_t>(offset)], data, size);
}

} // anonymous namespace

uint64 thunkAreaVA(const GuestImage& image)
{
    return alignUp(image.base + image.sizeOfImage, REFORGE_THUNK_STRIDE);
}

void checkImports(const GuestImage& image)
{
    for (std::size_t i = 0; i < image.imports.size(); i++)
    {
        const GuestImport& import = image.imports[i];
        if (findAdapterId(import.module.c_str(), import.symbol.c_str()) == ADAPTER_NONE)
            throw ReforgeError(RC_UNRESOLVED_IMPORT,
                               "unresolved import " + import.module + "!" + import.symbol +
                               " (no host adapter)");
    }
}

std::vector<uint8> buildMeta(const GuestImage& image,
                             const std::vector<ReforgeBlockEntry>& blocks)
{
    checkImports(image);
    if (image.image.size() != image.sizeOfImage)
        throw ReforgeError(RC_INTERNAL, "meta: image bytes do not match SizeOfImage");

    const uint64 importCount = image.imports.size();
    const uint64 thunkVA = thunkAreaVA(image);
    const uint64 stackLow = alignUp(thunkVA + importCount * REFORGE_THUNK_STRIDE, 16);
    const uint64 stackTop = stackLow + REFORGE_STACK_SIZE;

    // Strings: module and symbol names, deduplicated.
    std::vector<uint8> strings;
    std::map<std::string, uint32> stringOffsets;
    struct Offsets { uint32 module; uint32 symbol; };
    std::vector<Offsets> importNames;
    for (std::size_t i = 0; i < image.imports.size(); i++)
    {
        const std::string* names[2] = { &image.imports[i].module, &image.imports[i].symbol };
        uint32 result[2];
        for (int n = 0; n < 2; n++)
        {
            std::map<std::string, uint32>::const_iterator found = stringOffsets.find(*names[n]);
            if (found == stringOffsets.end())
            {
                uint32 offset = static_cast<uint32>(strings.size());
                strings.insert(strings.end(), names[n]->begin(), names[n]->end());
                strings.push_back(0);
                found = stringOffsets.insert(std::make_pair(*names[n], offset)).first;
            }
            result[n] = found->second;
        }
        Offsets o = { result[0], result[1] };
        importNames.push_back(o);
    }
    if (strings.empty())
        strings.push_back(0);

    // Layout of the blob.
    uint64 imageOffset = REFORGE_META_HEADER_SIZE;
    uint64 blockTableOffset = alignUp(imageOffset + image.sizeOfImage, 8);
    uint64 importTableOffset = blockTableOffset + blocks.size() * REFORGE_BLOCK_ENTRY_SIZE;
    uint64 stringsOffset = importTableOffset + importCount * REFORGE_IMPORT_ENTRY_SIZE;
    uint64 totalSize = alignUp(stringsOffset + strings.size(), 8);

    std::vector<uint8> blob(static_cast<std::size_t>(totalSize), 0);

    ReforgeMetaHeader h;
    std::memset(&h, 0, sizeof(h));
    h.magic = REFORGE_META_MAGIC;
    h.version = REFORGE_META_VERSION;
    h.totalSize = totalSize;
    h.guestBase = image.base;
    h.sizeOfImage = image.sizeOfImage;
    h.entryVA = image.entryVA;
    h.regionSize = stackTop - image.base;
    h.thunkVA = thunkVA;
    h.stackTopVA = stackTop;
    h.imageOffset = imageOffset;
    h.blockCount = blocks.size();
    h.blockTableOffset = blockTableOffset;
    h.importCount = importCount;
    h.importTableOffset = importTableOffset;
    h.stringsOffset = stringsOffset;
    h.stringsSize = strings.size();
    put(blob, 0, &h, sizeof(h));

    // Image snapshot with the IAT slots pointing at the thunks.
    if (!image.image.empty())
        put(blob, imageOffset, &image.image[0], image.image.size());
    for (std::size_t i = 0; i < image.imports.size(); i++)
    {
        uint64 va = image.imports[i].iatVA;
        if (va < image.base || va - image.base > image.sizeOfImage ||
            image.sizeOfImage - (va - image.base) < 8)
        {
            std::ostringstream s;
            s << "meta: IAT slot of " << image.imports[i].module << "!"
              << image.imports[i].symbol << " (0x" << std::hex << va << ") is outside the image";
            throw ReforgeError(RC_INTERNAL, s.str());
        }
        uint64 thunk = thunkVA + i * REFORGE_THUNK_STRIDE;
        put(blob, imageOffset + (va - image.base), &thunk, sizeof(thunk));
    }

    if (!blocks.empty())
        put(blob, blockTableOffset, &blocks[0], blocks.size() * sizeof(ReforgeBlockEntry));

    for (std::size_t i = 0; i < image.imports.size(); i++)
    {
        ReforgeImportEntry e;
        std::memset(&e, 0, sizeof(e));
        e.adapterId = findAdapterId(image.imports[i].module.c_str(),
                                    image.imports[i].symbol.c_str());
        e.moduleOffset = importNames[i].module;
        e.symbolOffset = importNames[i].symbol;
        e.iatVA = image.imports[i].iatVA;
        put(blob, importTableOffset + i * REFORGE_IMPORT_ENTRY_SIZE, &e, sizeof(e));
    }
    put(blob, stringsOffset, &strings[0], strings.size());
    return blob;
}

} // namespace reforge
