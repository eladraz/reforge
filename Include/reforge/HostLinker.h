#ifndef __TBA_REFORGE_HOSTLINKER_H
#define __TBA_REFORGE_HOSTLINKER_H

/*
 * HostLinker.h
 *
 * Runs host tools (the C++ driver as linker, the assembler) and links the
 * translated object with the runtime into an executable.
 */

#include <string>
#include <vector>

namespace reforge {

struct ProcessResult
{
    /* Exit status, or 128 + signal; 127 when the program cannot be started. */
    int exitCode;
    std::string out;
    std::string err;
    ProcessResult() : exitCode(-1) {}
};

/* Runs argv[0] (searched in PATH) with the arguments. Captures stdout and stderr. */
ProcessResult runProcess(const std::vector<std::string>& argv);

struct HostLinkConfig
{
    /* Host C++ compiler driver used as the linker. */
    std::string compiler;
    /* Host assembler (the --object-writer asm path). */
    std::string assembler;
    /* Archives linked after the object, in order. */
    std::vector<std::string> libraries;
    /* Extra arguments (before the object). */
    std::vector<std::string> flags;
};

/* The configuration baked in by CMake (compiler, libreforge_rt.a, xStl). */
HostLinkConfig defaultHostLinkConfig();

/* The command line that links objectPath into outputPath. */
std::vector<std::string> buildLinkCommand(const HostLinkConfig& config,
                                          const std::string& objectPath,
                                          const std::string& outputPath);

/*
 * Links the object with the runtime. A failure removes any partial output and
 * throws ReforgeError(RC_LINK_FAILURE) with the captured diagnostics.
 */
void linkExecutable(const HostLinkConfig& config, const std::string& objectPath,
                    const std::string& outputPath);

} // namespace reforge

#endif // __TBA_REFORGE_HOSTLINKER_H
