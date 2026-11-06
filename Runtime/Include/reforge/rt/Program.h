#ifndef __TBA_REFORGE_RT_PROGRAM_H
#define __TBA_REFORGE_RT_PROGRAM_H

/*
 * Program.h
 *
 * The runtime view of reforge_meta (layout documented in Include/reforge/Meta.h).
 * Program::parse() validates the whole blob once, so the rest of the runtime
 * can index the tables without further checks. Malformed blobs are rejected
 * with GuestFault, naming the broken field.
 */

#include <string>
#include <vector>
#include "xStl/types.h"
#include "reforge/Meta.h"
#include "reforge/rt/GuestFault.h"

namespace reforge {
namespace rt {

/* One import of the guest, as recorded by the translator. */
struct ProgramImport
{
    std::string module;
    std::string symbol;
    AdapterId adapter;
    /* Guest VA of the IAT slot that holds the thunk VA. */
    uint64 iatVA;
    /* Guest VA of the thunk (the pc value of a call to this import). */
    uint64 thunkVA;
};

class Program
{
public:
    /*
     * Validates the blob of size metaSize and copies what the runtime needs.
     * Throws GuestFault on any inconsistency.
     */
    static Program parse(const uint8* meta, uint64 metaSize);

    uint64 guestBase;
    uint64 sizeOfImage;
    uint64 entryVA;
    uint64 regionSize;
    uint64 thunkVA;
    uint64 stackTopVA;
    /* Image bytes, valid for sizeOfImage bytes (points into the meta blob). */
    const uint8* image;

    std::vector<ReforgeBlockEntry> blocks;
    std::vector<ProgramImport> imports;

    /* Finds the host offset of a recovered guest block. */
    bool lookupBlock(uint64 guestVA, uint64& hostOffset) const;

    /*
     * If pc is the thunk of an import, returns true and the import index.
     * Only thunk-aligned addresses inside the thunk area match.
     */
    bool lookupThunk(uint64 pc, uint64& importIndex) const;

    /* Size of the thunk area in bytes (16 per import). */
    uint64 getThunkAreaSize() const;

private:
    Program() {}
};

} // namespace rt
} // namespace reforge

#endif // __TBA_REFORGE_RT_PROGRAM_H
