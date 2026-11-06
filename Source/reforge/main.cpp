/*
 * main.cpp
 *
 * Entry point of the reforge driver. Parses the command line and runs the
 * translation pipeline:
 *
 *   load PE -> flow recovery + lift -> legalize + lower -> object -> host link
 *
 * The stages live in Translator.cpp; the lowering is dismount's (Amd64Lowerer).
 */
#include <exception>
#include <iostream>
#include <string>
#include "reforge/Amd64Lowerer.h"
#include "reforge/Diagnostics.h"
#include "reforge/Options.h"
#include "reforge/Translator.h"

namespace reforge {

namespace {

void logConfiguration(const Options& options)
{
    logInfo(std::string("input ") + options.input + ", source " +
            toString(options.sourceFormat) + "/" + toString(options.sourceIsa) +
            "/" + toString(options.sourceAbi) + ", target " +
            toString(options.targetFormat) + "/" + toString(options.targetIsa) +
            "/" + toString(options.targetOs) + "/" + toString(options.targetAbi) +
            ", object writer " + toString(options.objectWriter));
    if (!options.noLink)
        logInfo("output " + options.output);
}

/* Runs the pipeline. Throws ReforgeError on failure. */
void runPipeline(const Options& options)
{
    Amd64Lowerer lowerer;
    translate(options, &lowerer);
}

} // anonymous namespace

} // namespace reforge

int main(int argc, const char* const* argv)
{
    using namespace reforge;
    try
    {
        Options options = parseOptions(argc, argv);
        if (options.helpRequested)
        {
            printUsage(std::cout);
            return RC_OK;
        }
        if (options.verbose)
            logConfiguration(options);
        runPipeline(options);
        return RC_OK;
    }
    catch (const ReforgeError& e)
    {
        logError(e.what());
        if (e.getExitCode() == RC_USAGE)
            std::cerr << "reforge: run 'reforge --help' for the options" << std::endl;
        return e.getExitCode();
    }
    catch (const std::exception& e)
    {
        logError(std::string("internal error: ") + e.what());
        return RC_INTERNAL;
    }
}
