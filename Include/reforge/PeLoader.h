#ifndef __TBA_REFORGE_PELOADER_H
#define __TBA_REFORGE_PELOADER_H

/*
 * PeLoader.h
 *
 * Loads a Windows-format RISC-V64 console image (PE32+, IMAGE_FILE_MACHINE_RISCV64)
 * into a GuestImage. The loader validates the container and nothing else: it
 * does not decode code, resolve imports against adapters, or recover control
 * flow. Malformed input is reported as ReforgeError(RC_MALFORMED_INPUT) with a
 * message that names the offending field, offset or RVA.
 *
 * The three address kinds are kept apart by type inside the loader:
 *   FileOffset - byte offset into the input file
 *   Rva        - offset relative to the image base (as stored in the PE headers)
 *   Va         - guest virtual address (image base + RVA)
 */

#include <string>
#include <vector>
#include "xStl/types.h"
#include "reforge/GuestImage.h"

namespace reforge {

/* Byte offset into the input file. */
struct FileOffset
{
    explicit FileOffset(uint64 v = 0) : value(v) {}
    uint64 value;
};

/* Relative virtual address: an offset from the image base. */
struct Rva
{
    explicit Rva(uint64 v = 0) : value(v) {}
    uint64 value;
};

/* Guest virtual address: image base plus RVA. */
struct Va
{
    explicit Va(uint64 v = 0) : value(v) {}
    uint64 value;
};

/*
 * Loads the PE image in the given file bytes.
 *
 * Checks, in order: DOS/PE signatures, the file is long enough for the headers
 * and section table, Machine == 0x5064 (RISCV64), Magic == 0x20b (PE32+),
 * Subsystem == IMAGE_SUBSYSTEM_WINDOWS_CUI, SizeOfImage <= 256 MiB, section
 * alignment is a power of two, every section lies inside the image and its raw
 * data lies inside the file, no two sections overlap, no section is both
 * writable and executable, the entry point lies inside an executable section,
 * the base relocation directory lies inside the image, and the import table is
 * well formed (parsed by cNtDirImport).
 *
 * Throws ReforgeError(RC_MALFORMED_INPUT, ...) on any failure.
 */
GuestImage loadPeImage(const std::vector<uint8>& file);

/*
 * Reads the file and loads it with loadPeImage(). Throws
 * ReforgeError(RC_MALFORMED_INPUT, ...) if the file cannot be read.
 */
GuestImage loadPeFile(const std::string& path);

} // namespace reforge

#endif // __TBA_REFORGE_PELOADER_H
