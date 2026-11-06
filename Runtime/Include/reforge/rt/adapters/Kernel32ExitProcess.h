#ifndef __TBA_REFORGE_RT_ADAPTERS_KERNEL32EXITPROCESS_H
#define __TBA_REFORGE_RT_ADAPTERS_KERNEL32EXITPROCESS_H

/*
 * Kernel32ExitProcess.h
 *
 * kernel32.dll!ExitProcess(UINT uExitCode). Ends the program: the runtime
 * returns uExitCode & 0xff to the host as the process status.
 */

#include "reforge/rt/Adapter.h"

namespace reforge {
namespace rt {

/* The AdapterFn for ADAPTER_KERNEL32_EXITPROCESS. */
void kernel32ExitProcessAdapter(AdapterContext& context);

} // namespace rt
} // namespace reforge

#endif // __TBA_REFORGE_RT_ADAPTERS_KERNEL32EXITPROCESS_H
