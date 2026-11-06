/*
 * Adapter.cpp
 *
 * The argument, result and trace helpers of AdapterContext.
 */

#include "reforge/rt/Adapter.h"

namespace reforge {
namespace rt {

AdapterContext::AdapterContext(GuestState& state, GuestMemory& memory,
                               OutputSink& output, bool traceEnabled,
                               const ProgramImport& import, uint64 callSite)
    : state(state),
      memory(memory),
      output(output),
      import(import),
      callSite(callSite),
      m_traceEnabled(traceEnabled),
      m_terminated(false),
      m_exitStatus(0)
{
}

uint64 AdapterContext::arg(unsigned index) const
{
    return state.x[10 + index];
}

void AdapterContext::setResult(uint64 value)
{
    state.x[10] = value;
}

void AdapterContext::terminate(uint32 status)
{
    m_terminated = true;
    m_exitStatus = status;
}

void AdapterContext::trace(const std::string& message) const
{
    if (!m_traceEnabled)
        return;
    std::string line = "reforge: trace: " + message + "\n";
    output.writeStderr(line.data(), line.size());
}

} // namespace rt
} // namespace reforge
