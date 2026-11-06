#ifndef __TBA_REFORGE_RT_RUNTIME_H
#define __TBA_REFORGE_RT_RUNTIME_H

/*
 * Runtime.h
 *
 * Runs a translated guest program. The loop alternates between translated
 * code (entered through reforge_enter) and the host: calls to import thunks
 * go to the adapters, dynamic jumps are resolved through the block table, and
 * the return of the entry function ends the program with its exit status.
 */

#include <cstddef>
#include "xStl/types.h"
#include "reforge/rt/Adapter.h"

namespace reforge {
namespace rt {

/*
 * Runs the program described by the metadata blob (meta, metaSize) whose
 * translated code starts at text. Returns the process exit status: the
 * entry's return value & 0xff, or the status given by ExitProcess.
 * Throws GuestFault for runtime faults; the caller reports them.
 *
 * trace: when true, import dispatch is logged to output's stderr.
 */
int runGuestProgram(const uint8* meta, uint64 metaSize, const uint8* text,
                    OutputSink& output, bool trace);

/*
 * The diagnostic the fault handler writes (without allocation or I/O, so it is
 * async-signal-safe): a line "reforge-rt: runtime error: ..." naming the host
 * address and the guest address. Returns the length written; the text is
 * truncated to capacity - 1 bytes.
 */
std::size_t formatFaultLine(char* buffer, std::size_t capacity, int signal, uint64 hostAddress,
                            uint64 bias);

} // namespace rt
} // namespace reforge

#endif // __TBA_REFORGE_RT_RUNTIME_H
