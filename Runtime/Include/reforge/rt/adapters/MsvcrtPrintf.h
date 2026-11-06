#ifndef __TBA_REFORGE_RT_ADAPTERS_MSVCRTPRINTF_H
#define __TBA_REFORGE_RT_ADAPTERS_MSVCRTPRINTF_H

/*
 * MsvcrtPrintf.h
 *
 * msvcrt.dll!printf. Supported: literal text, %s and %%. Any other conversion
 * is rejected with GuestFault (nothing is printed in that case). The result
 * is the number of bytes written, in a0.
 */

#include <cstddef>
#include <string>
#include "xStl/types.h"
#include "reforge/rt/Adapter.h"
#include "reforge/rt/GuestMemory.h"

namespace reforge {
namespace rt {

/* Longest format string and longest %s argument accepted, in bytes. */
const uint64 PRINTF_MAX_FORMAT = 64 * 1024;
const uint64 PRINTF_MAX_STRING = 1024 * 1024;

/*
 * Formats fmtVA with the variadic arguments args[0..argCount) (guest a1..a7).
 * Throws GuestFault for unsupported conversions, missing arguments and bad
 * guest pointers. callSite is only used in the error message.
 */
std::string formatPrintf(const GuestMemory& memory, uint64 fmtVA,
                         const uint64* args, std::size_t argCount,
                         uint64 callSite);

/* The AdapterFn for ADAPTER_MSVCRT_PRINTF. */
void msvcrtPrintfAdapter(AdapterContext& context);

} // namespace rt
} // namespace reforge

#endif // __TBA_REFORGE_RT_ADAPTERS_MSVCRTPRINTF_H
