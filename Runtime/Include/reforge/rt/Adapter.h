#ifndef __TBA_REFORGE_RT_ADAPTER_H
#define __TBA_REFORGE_RT_ADAPTER_H

/*
 * Adapter.h
 *
 * Host side of guest imports. A call to an import jumps to its thunk; the
 * runtime then calls the adapter registered for the import's adapter id with
 * an AdapterContext. The adapter reads the guest arguments (a0..a7, LP64),
 * validates pointers through GuestMemory, performs the host work and leaves
 * the return value in a0. Adapters never call printf, iostreams or raw system
 * calls: output goes through OutputSink, which the runtime implements with the
 * xStl standard streams.
 */

#include <string>
#include "xStl/types.h"
#include "reforge/GuestState.h"
#include "reforge/rt/GuestMemory.h"
#include "reforge/rt/Program.h"

namespace reforge {
namespace rt {

/* Destination of the process output and diagnostics. */
class OutputSink
{
public:
    virtual ~OutputSink() {}

    /* Guest-visible standard output (the bytes the program prints). */
    virtual void writeStdout(const void* data, uint64 length) = 0;

    /* Diagnostics and traces. Never used for program output. */
    virtual void writeStderr(const void* data, uint64 length) = 0;
};

/* OutputSink backed by the xStl standard streams (cFile::writeAll). */
class StandardStreamSink : public OutputSink
{
public:
    virtual void writeStdout(const void* data, uint64 length);
    virtual void writeStderr(const void* data, uint64 length);
};

/* What an adapter sees when it is called for one guest import. */
class AdapterContext
{
public:
    AdapterContext(GuestState& state, GuestMemory& memory, OutputSink& output,
                   bool traceEnabled, const ProgramImport& import,
                   uint64 callSite);

    GuestState& state;
    GuestMemory& memory;
    OutputSink& output;
    const ProgramImport& import;
    /* Guest address of the call instruction (return address - 4). */
    uint64 callSite;

    /* Argument i (0 = a0 .. 7 = a7) of the guest call. */
    uint64 arg(unsigned index) const;

    /* Sets the return value (a0). */
    void setResult(uint64 value);

    /* Ends the program with the given exit status when the adapter returns. */
    void terminate(uint32 status);
    bool isTerminated() const { return m_terminated; }
    uint32 getExitStatus() const { return m_exitStatus; }

    /* "reforge: trace: <message>" on stderr when tracing is enabled. */
    void trace(const std::string& message) const;

private:
    bool m_traceEnabled;
    bool m_terminated;
    uint32 m_exitStatus;
};

/* An adapter: performs one guest import. Throws GuestFault on bad input. */
typedef void (*AdapterFn)(AdapterContext& context);

} // namespace rt
} // namespace reforge

#endif // __TBA_REFORGE_RT_ADAPTER_H
