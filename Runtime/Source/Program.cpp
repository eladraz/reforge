/*
 * Program.cpp
 *
 * Validation of the reforge_meta blob. Every check below corresponds to a
 * condition the translator guarantees; a failure means a corrupt or mismatched
 * blob, which is reported as a runtime fault rather than being trusted.
 */

#include <cstring>
#include "reforge/Meta.h"
#include "reforge/rt/AdapterRegistry.h"
#include "reforge/rt/Program.h"

namespace reforge {
namespace rt {

namespace {

/* Limits that keep the allocation of the guest region sane. */
const uint64 MAX_IMAGE_SIZE = 256ull * 1024 * 1024;
const uint64 MAX_REGION_SIZE = 512ull * 1024 * 1024;

uint64 readU64(const uint8* p)
{
    uint64 v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

uint32 readU32(const uint8* p)
{
    uint32 v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

[[noreturn]] void malformed(const char* what)
{
    throw GuestFault(std::string("malformed reforge_meta: ") + what);
}

/* True when [offset, offset+length) lies inside [0, total). */
bool rangeInside(uint64 offset, uint64 length, uint64 total)
{
    return offset <= total && length <= total - offset;
}

/* Table of count entries of entrySize bytes at offset, inside the blob. */
bool tableInside(uint64 offset, uint64 count, uint64 entrySize, uint64 total)
{
    if (count > total / entrySize)
        return false;
    return rangeInside(offset, count * entrySize, total);
}

/* Reads the NUL-terminated name at offset, inside the string area. */
std::string readName(const uint8* blob, uint64 stringsOffset, uint64 stringsSize,
                     uint32 offset, const char* what)
{
    if (offset >= stringsSize)
        malformed(what);
    const char* begin = reinterpret_cast<const char*>(blob + stringsOffset);
    for (uint64 i = offset; i < stringsSize; i++)
    {
        if (begin[i] == '\0')
            return std::string(begin + offset, static_cast<std::size_t>(i - offset));
    }
    malformed(what);
}

} // anonymous namespace

Program Program::parse(const uint8* meta, uint64 metaSize)
{
    if (metaSize < REFORGE_META_HEADER_SIZE)
        malformed("blob shorter than the header");

    ReforgeMetaHeader h;
    std::memcpy(&h, meta, sizeof(h));
    if (h.magic != REFORGE_META_MAGIC)
        malformed("bad magic");
    if (h.version != REFORGE_META_VERSION)
        malformed("unsupported version");
    if (h.totalSize != metaSize)
        malformed("total size does not match the blob");
    if (h.reserved != 0)
        malformed("reserved header field is not zero");

    // Guest region: image at guestBase, then thunks, then the stack at the top.
    if (h.sizeOfImage == 0 || h.sizeOfImage > MAX_IMAGE_SIZE)
        malformed("image size out of range");
    if (h.regionSize < h.sizeOfImage || h.regionSize > MAX_REGION_SIZE)
        malformed("region size out of range");
    if (h.guestBase + h.regionSize < h.guestBase)
        malformed("guest region wraps around the address space");
    uint64 regionEnd = h.guestBase + h.regionSize;

    if (h.entryVA < h.guestBase || h.entryVA >= h.guestBase + h.sizeOfImage)
        malformed("entry point is outside the image");

    if (h.importCount > metaSize / REFORGE_IMPORT_ENTRY_SIZE)
        malformed("import count is larger than the blob");
    uint64 thunkBytes = h.importCount * REFORGE_THUNK_STRIDE;
    if (h.thunkVA % REFORGE_THUNK_STRIDE != 0)
        malformed("thunk area is not aligned");
    if (h.thunkVA < h.guestBase + h.sizeOfImage ||
        h.thunkVA > regionEnd || thunkBytes > regionEnd - h.thunkVA)
        malformed("thunk area is outside the guest region");

    if (h.stackTopVA % 16 != 0)
        malformed("stack top is not 16-byte aligned");
    if (h.stackTopVA > regionEnd || h.stackTopVA < h.guestBase + REFORGE_STACK_SIZE)
        malformed("stack top is outside the guest region");
    uint64 stackLow = h.stackTopVA - REFORGE_STACK_SIZE;
    if (stackLow < h.guestBase + h.sizeOfImage || stackLow < h.thunkVA + thunkBytes)
        malformed("stack overlaps the image or the thunk area");

    // Blob tables.
    if (!rangeInside(h.imageOffset, h.sizeOfImage, metaSize))
        malformed("image is outside the blob");
    if (!tableInside(h.blockTableOffset, h.blockCount, REFORGE_BLOCK_ENTRY_SIZE, metaSize))
        malformed("block table is outside the blob");
    if (!tableInside(h.importTableOffset, h.importCount, REFORGE_IMPORT_ENTRY_SIZE, metaSize))
        malformed("import table is outside the blob");
    if (!rangeInside(h.stringsOffset, h.stringsSize, metaSize))
        malformed("string area is outside the blob");

    Program p;
    p.guestBase = h.guestBase;
    p.sizeOfImage = h.sizeOfImage;
    p.entryVA = h.entryVA;
    p.regionSize = h.regionSize;
    p.thunkVA = h.thunkVA;
    p.stackTopVA = h.stackTopVA;
    p.image = meta + h.imageOffset;

    // Block table: strictly ascending guest addresses inside the image.
    p.blocks.reserve(static_cast<std::size_t>(h.blockCount));
    for (uint64 i = 0; i < h.blockCount; i++)
    {
        const uint8* e = meta + h.blockTableOffset + i * REFORGE_BLOCK_ENTRY_SIZE;
        ReforgeBlockEntry block;
        block.guestVA = readU64(e);
        block.hostOffset = readU64(e + 8);
        if (block.guestVA < h.guestBase || block.guestVA >= h.guestBase + h.sizeOfImage)
            malformed("block outside the image");
        if (!p.blocks.empty() && block.guestVA <= p.blocks.back().guestVA)
            malformed("block table is not strictly ascending");
        p.blocks.push_back(block);
    }

    // Import table: adapters must exist, names must be terminated.
    p.imports.reserve(static_cast<std::size_t>(h.importCount));
    for (uint64 i = 0; i < h.importCount; i++)
    {
        const uint8* e = meta + h.importTableOffset + i * REFORGE_IMPORT_ENTRY_SIZE;
        ProgramImport imp;
        uint32 adapterId = readU32(e);
        uint32 moduleOffset = readU32(e + 4);
        uint32 symbolOffset = readU32(e + 8);
        imp.adapter = static_cast<AdapterId>(adapterId);
        imp.iatVA = readU64(e + 16);
        imp.thunkVA = h.thunkVA + i * REFORGE_THUNK_STRIDE;
        if (lookupAdapter(adapterId) == nullptr)
            malformed("import has no adapter");
        imp.module = readName(meta, h.stringsOffset, h.stringsSize, moduleOffset,
                              "import module name");
        imp.symbol = readName(meta, h.stringsOffset, h.stringsSize, symbolOffset,
                              "import symbol name");
        p.imports.push_back(imp);
    }

    return p;
}

bool Program::lookupBlock(uint64 guestVA, uint64& hostOffset) const
{
    std::size_t lo = 0;
    std::size_t hi = blocks.size();
    while (lo < hi)
    {
        std::size_t mid = lo + (hi - lo) / 2;
        if (blocks[mid].guestVA == guestVA)
        {
            hostOffset = blocks[mid].hostOffset;
            return true;
        }
        if (blocks[mid].guestVA < guestVA)
            lo = mid + 1;
        else
            hi = mid;
    }
    return false;
}

bool Program::lookupThunk(uint64 pc, uint64& importIndex) const
{
    uint64 size = getThunkAreaSize();
    if (size == 0 || pc < thunkVA || pc - thunkVA >= size)
        return false;
    if ((pc - thunkVA) % REFORGE_THUNK_STRIDE != 0)
        return false;
    importIndex = (pc - thunkVA) / REFORGE_THUNK_STRIDE;
    return true;
}

uint64 Program::getThunkAreaSize() const
{
    return static_cast<uint64>(imports.size()) * REFORGE_THUNK_STRIDE;
}

} // namespace rt
} // namespace reforge
