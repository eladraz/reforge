#ifndef __TBA_REFORGE_OPTIONS_H
#define __TBA_REFORGE_OPTIONS_H

/*
 * Options.h
 *
 * Command line of the reforge driver.
 */

#include <ostream>
#include <string>
#include "reforge/Config.h"

namespace reforge {

struct Options
{
    bool helpRequested;
    bool verbose;
    bool noLink;

    std::string input;
    std::string output;
    std::string emitObject;
    std::string dumpDir;

    SourceFormat sourceFormat;
    SourceIsa sourceIsa;
    SourceAbi sourceAbi;
    TargetFormat targetFormat;
    TargetIsa targetIsa;
    TargetOs targetOs;
    TargetAbi targetAbi;
    ObjectWriter objectWriter;

    bool dumpDisasm;
    bool dumpIr;
    bool dumpLegalized;
    bool dumpTarget;
    bool dumpImports;
    bool dumpAddrmap;
};

/*
 * Parses argv (argv[0] is the program name). Throws ReforgeError with
 * RC_USAGE for unknown options, missing values and unsupported values.
 */
Options parseOptions(int argc, const char* const* argv);

/* Prints the command line help. */
void printUsage(std::ostream& out);

} // namespace reforge

#endif // __TBA_REFORGE_OPTIONS_H
