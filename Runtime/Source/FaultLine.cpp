/*
 * FaultLine.cpp
 *
 * The fault diagnostic of the runtime (see Runtime.cpp, guestFaultHandler).
 * It is kept in its own translation unit with no dependency on the translated
 * code, so that the tests can link it. Nothing here allocates or does I/O: the
 * handler runs in signal context.
 */

#include <csignal>
#include <cstddef>
#include <string>
#include "xStl/types.h"
#include "reforge/rt/Runtime.h"

namespace reforge {
namespace rt {

namespace {

void appendText(char* buffer, std::size_t capacity, std::size_t& at, const char* text)
{
    for (; *text != 0 && at + 1 < capacity; text++)
        buffer[at++] = *text;
}

// Appends 0x and the value in hexadecimal, without leading zeros.
void appendHex(char* buffer, std::size_t capacity, std::size_t& at, uint64 value)
{
    static const char digits[] = "0123456789abcdef";
    appendText(buffer, capacity, at, "0x");
    bool started = false;
    for (int shift = 60; shift >= 0 && at + 1 < capacity; shift -= 4)
    {
        unsigned nibble = static_cast<unsigned>((value >> shift) & 15);
        if (nibble != 0 || started || shift == 0)
        {
            buffer[at++] = digits[nibble];
            started = true;
        }
    }
}

} // anonymous namespace

std::size_t formatFaultLine(char* buffer, std::size_t capacity, int signal, uint64 hostAddress,
                            uint64 bias)
{
    std::size_t at = 0;
    appendText(buffer, capacity, at, "reforge-rt: runtime error: ");
    appendText(buffer, capacity, at, signal == SIGBUS ? "bus error" : "segmentation fault");
    appendText(buffer, capacity, at, " at host address ");
    appendHex(buffer, capacity, at, hostAddress);
    appendText(buffer, capacity, at, " (guest address ");
    appendHex(buffer, capacity, at, hostAddress - bias);
    appendText(buffer, capacity, at,
               " if this was a translated load or store outside guest memory)\n");
    return at;
}

} // namespace rt
} // namespace reforge
