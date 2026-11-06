/*
 * Kernel32ExitProcess.cpp
 *
 * kernel32.dll!ExitProcess. The guest exit code is kept as a 32-bit value;
 * the process status is that value & 0xff (what a POSIX exit status can carry).
 */

#include <string>
#include "reforge/rt/adapters/Kernel32ExitProcess.h"

namespace reforge {
namespace rt {

void kernel32ExitProcessAdapter(AdapterContext& context)
{
    uint32 code = static_cast<uint32>(context.arg(0));
    context.trace("kernel32.dll!ExitProcess(" + std::to_string(code) +
                  ") -> runtime termination");
    context.terminate(code);
}

} // namespace rt
} // namespace reforge
