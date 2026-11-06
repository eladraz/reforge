/*
 * StandardStreams.cpp
 *
 * The only place where the runtime writes to the process streams. Both
 * directions go through the xStl standard stream objects and cFile::writeAll,
 * which retries short writes and EINTR.
 */

#include <string>
#include "xStl/os/file.h"
#include "reforge/rt/Adapter.h"

namespace reforge {
namespace rt {

namespace {

/* Largest chunk handed to cFile::writeAll (its length parameter is 32-bit). */
const uint64 WRITE_CHUNK = 1024 * 1024 * 1024;

void writeChunked(StandardStream stream, const void* data, uint64 length)
{
    const uint8* p = static_cast<const uint8*>(data);
    cFile& file = cFile::getStandardStream(stream);
    while (length > 0)
    {
        uint64 chunk = length < WRITE_CHUNK ? length : WRITE_CHUNK;
        file.writeAll(p, static_cast<uint>(chunk));
        p += chunk;
        length -= chunk;
    }
}

} // anonymous namespace

void StandardStreamSink::writeStdout(const void* data, uint64 length)
{
    writeChunked(STANDARD_OUTPUT, data, length);
}

void StandardStreamSink::writeStderr(const void* data, uint64 length)
{
    writeChunked(STANDARD_ERROR, data, length);
}

} // namespace rt
} // namespace reforge
