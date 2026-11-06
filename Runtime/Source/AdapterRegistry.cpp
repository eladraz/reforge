/*
 * AdapterRegistry.cpp
 *
 * Adapter id -> host function. The ids are defined in Include/reforge/Meta.h.
 */

#include "reforge/Meta.h"
#include "reforge/rt/AdapterRegistry.h"
#include "reforge/rt/adapters/Kernel32ExitProcess.h"
#include "reforge/rt/adapters/MsvcrtPrintf.h"

namespace reforge {
namespace rt {

AdapterFn lookupAdapter(uint32 adapterId)
{
    switch (adapterId)
    {
    case ADAPTER_MSVCRT_PRINTF:
        return &msvcrtPrintfAdapter;
    case ADAPTER_KERNEL32_EXITPROCESS:
        return &kernel32ExitProcessAdapter;
    default:
        return nullptr;
    }
}

} // namespace rt
} // namespace reforge
