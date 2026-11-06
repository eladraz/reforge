#ifndef __TBA_REFORGE_GUESTIMAGE_H
#define __TBA_REFORGE_GUESTIMAGE_H

/*
 * GuestImage.h
 *
 * In-memory description of a loaded guest image (a PE32+ RISC-V64 module).
 * Addresses are kept apart by kind: a file offset is an offset into the input
 * file, an RVA is relative to the image base, and a VA is a guest virtual
 * address (image base + RVA). The loader that fills this in is not written yet.
 */

#include <string>
#include <vector>
#include "xStl/types.h"

namespace reforge {

/* A section of the guest image, with its memory permissions. */
struct GuestSection
{
    std::string name;
    /* RVA of the first byte of the section. */
    uint64 rva;
    uint64 virtualSize;
    /* Offset of the raw data in the input file. */
    uint64 fileOffset;
    uint64 rawSize;
    bool readable;
    bool writable;
    bool executable;
};

/* One imported function, keyed by module and symbol name. */
struct GuestImport
{
    /* Module name, lower case (e.g. "msvcrt.dll"). */
    std::string module;
    /* Imported symbol name (e.g. "printf"). */
    std::string symbol;
    /* VA of the import address table slot that holds the function pointer. */
    uint64 iatVA;
};

struct GuestImage
{
    /* Preferred image base (VA). */
    uint64 base;
    /* SizeOfImage: size of the image in memory, starting at base. */
    uint64 sizeOfImage;
    /* VA of the entry point. */
    uint64 entryVA;
    /* The image laid out by RVA: sizeOfImage bytes, zero filled. */
    std::vector<uint8> image;
    std::vector<GuestSection> sections;
    std::vector<GuestImport> imports;
};

} // namespace reforge

#endif // __TBA_REFORGE_GUESTIMAGE_H
