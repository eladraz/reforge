#ifndef __TBA_REFORGE_DIAGNOSTICS_H
#define __TBA_REFORGE_DIAGNOSTICS_H

/*
 * Diagnostics.h
 *
 * Process exit codes and error reporting. Diagnostics always go to stderr;
 * stdout is reserved for the output the user asked for (only --help today).
 */

#include <stdexcept>
#include <string>
#include "xStl/types.h"

// xStl defines RC_OK as a macro (xStl/types.h); the enumerator below must win
// whatever the include order of the translation unit.
#undef RC_OK

namespace reforge {

/* Process exit status. */
enum ExitCode
{
    RC_OK = 0,
    /* Internal error, or a pipeline stage that is not implemented yet. */
    RC_INTERNAL = 1,
    /* Bad command line: unknown option, missing value, unsupported target. */
    RC_USAGE = 2,
    /* Input cannot be read or is not a well-formed PE32+ RISC-V image. */
    RC_MALFORMED_INPUT = 3,
    /* Reachable guest instruction that cannot be decoded or lifted. */
    RC_UNSUPPORTED_INSTRUCTION = 4,
    /* Guest import that has no host adapter. */
    RC_UNRESOLVED_IMPORT = 5,
    /* Guest control flow that cannot be resolved statically. */
    RC_UNRESOLVED_CONTROL_FLOW = 6,
    /* The host linker failed to produce the output executable. */
    RC_LINK_FAILURE = 7
};

/*
 * An error that terminates the translation with the given exit status.
 * The message is printed by main() and must name the guest address and the
 * offending instruction or import when one is involved.
 */
class ReforgeError : public std::runtime_error
{
public:
    ReforgeError(ExitCode code, const std::string& message);

    ExitCode getExitCode() const;

private:
    ExitCode m_code;
};

/* Informational message ("reforge: <message>") on stderr. */
void logInfo(const std::string& message);

/* Error message ("reforge: error: <message>") on stderr. */
void logError(const std::string& message);

} // namespace reforge

#endif // __TBA_REFORGE_DIAGNOSTICS_H
