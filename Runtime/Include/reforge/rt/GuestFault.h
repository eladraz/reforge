#ifndef __TBA_REFORGE_RT_GUESTFAULT_H
#define __TBA_REFORGE_RT_GUESTFAULT_H

/*
 * GuestFault.h
 *
 * The error raised by the runtime when the guest does something that cannot
 * be given a meaning on the host: a pointer outside guest memory, an unknown
 * guest pc, an unsupported printf conversion, a trap in translated code. The
 * runtime turns it into a diagnostic on stderr and the exit status
 * REFORGE_RUNTIME_FAULT_EXIT. Nothing is printed on stdout for the failing
 * call.
 */

#include <stdexcept>
#include <string>
#include "xStl/types.h"

namespace reforge {
namespace rt {

class GuestFault : public std::runtime_error
{
public:
    explicit GuestFault(const std::string& message)
        : std::runtime_error(message)
    {
    }
};

/* Lower-case hexadecimal of a value without leading zeros ("0x" prefix). */
inline std::string guestHex(uint64 value)
{
    static const char digits[] = "0123456789abcdef";
    std::string out;
    do
    {
        out.insert(out.begin(), digits[value & 0xf]);
        value >>= 4;
    } while (value != 0);
    return "0x" + out;
}

} // namespace rt
} // namespace reforge

#endif // __TBA_REFORGE_RT_GUESTFAULT_H
