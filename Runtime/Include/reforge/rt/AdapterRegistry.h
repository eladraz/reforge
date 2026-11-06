#ifndef __TBA_REFORGE_RT_ADAPTERREGISTRY_H
#define __TBA_REFORGE_RT_ADAPTERREGISTRY_H

/*
 * AdapterRegistry.h
 *
 * Maps the adapter ids of Meta.h (AdapterId) to the host functions that
 * implement them. Adding an import means adding a row to adapterKeyTable() in
 * Meta.h and a row here.
 */

#include "xStl/types.h"
#include "reforge/rt/Adapter.h"

namespace reforge {
namespace rt {

/* The adapter for an id, or nullptr if the id is unknown. */
AdapterFn lookupAdapter(uint32 adapterId);

} // namespace rt
} // namespace reforge

#endif // __TBA_REFORGE_RT_ADAPTERREGISTRY_H
