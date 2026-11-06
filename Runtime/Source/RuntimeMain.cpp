/*
 * RuntimeMain.cpp
 *
 * main() of every translated executable. It reads the reforge_meta and
 * reforge_text symbols emitted by the translator, runs the program and
 * reports runtime faults on stderr. REFORGE_TRACE=1 (any non-empty value
 * other than "0") enables import tracing on stderr; stdout carries only the
 * guest's output.
 */

#include <cstdlib>
#include <cstring>
#include <string>
#include "xStl/except/exception.h"
#include "reforge/Meta.h"
#include "reforge/rt/Adapter.h"
#include "reforge/rt/GuestFault.h"
#include "reforge/rt/Runtime.h"

// Symbols emitted by the translated object.
extern "C" const unsigned char reforge_meta[];
extern "C" const unsigned char reforge_text[];

namespace {

bool isTraceRequested()
{
    const char* value = std::getenv("REFORGE_TRACE");
    return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

void reportError(reforge::rt::OutputSink& output, const std::string& message)
{
    std::string line = "reforge-rt: runtime error: " + message + "\n";
    try
    {
        output.writeStderr(line.data(), line.size());
    }
    catch (...)
    {
        // Nothing else can be reported; the exit status still tells the caller.
    }
}

} // anonymous namespace

int main(int, char**)
{
    using namespace reforge;
    rt::StandardStreamSink output;
    try
    {
        ReforgeMetaHeader header;
        std::memcpy(&header, reforge_meta, sizeof(header));
        return rt::runGuestProgram(reforge_meta, header.totalSize, reforge_text,
                                   output, isTraceRequested());
    }
    catch (const rt::GuestFault& e)
    {
        reportError(output, e.what());
        return REFORGE_RUNTIME_FAULT_EXIT;
    }
    catch (const cException&)
    {
        reportError(output, "host I/O failed");
        return REFORGE_RUNTIME_FAULT_EXIT;
    }
    catch (const std::exception& e)
    {
        reportError(output, std::string("internal error: ") + e.what());
        return REFORGE_RUNTIME_FAULT_EXIT;
    }
}
