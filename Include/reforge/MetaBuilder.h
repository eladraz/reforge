#ifndef __TBA_REFORGE_METABUILDER_H
#define __TBA_REFORGE_METABUILDER_H

/*
 * MetaBuilder.h
 *
 * Builds the reforge_meta blob (layout: Meta.h, section 2) from the guest
 * image and the block table. Every import gets an adapter id and a thunk VA
 * after the image; the thunk VA is written into the IAT slot of the image
 * snapshot embedded in the blob.
 */

#include <vector>
#include "xStl/types.h"
#include "reforge/GuestImage.h"
#include "reforge/Meta.h"

namespace reforge {

/* Guest VA of the thunk of import index (see Meta.h). */
uint64 thunkAreaVA(const GuestImage& image);

/*
 * Throws ReforgeError(RC_UNRESOLVED_IMPORT) with "unresolved import M!S" for an
 * import without an adapter, and RC_INTERNAL for an IAT slot outside the image.
 */
std::vector<uint8> buildMeta(const GuestImage& image,
                             const std::vector<ReforgeBlockEntry>& blocks);

/* Checks every import of the image against the adapter table (exit 5). */
void checkImports(const GuestImage& image);

} // namespace reforge

#endif // __TBA_REFORGE_METABUILDER_H
