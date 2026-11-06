#ifndef __TBA_REFORGE_RT_GUESTMEMORY_H
#define __TBA_REFORGE_RT_GUESTMEMORY_H

/*
 * GuestMemory.h
 *
 * The guest address space as seen by the host. One host region backs the
 * guest range [base, base + size). Translated code addresses it with
 * host = bias + guestVA (bias = r14), and adapters use toHost()/read()/write()
 * so that every guest pointer is bounds checked before it is dereferenced.
 *
 * Ranges can be forbidden for data access (the thunk area holds no data, only
 * call targets). Any access that is outside the region or overlaps a forbidden
 * range throws GuestFault, naming the guest address.
 */

#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include "xStl/types.h"
#include "reforge/rt/GuestFault.h"

namespace reforge {
namespace rt {

class GuestMemory
{
public:
    /* Allocates and zeroes size bytes of host memory for [base, base+size). */
    GuestMemory(uint64 base, uint64 size);

    uint64 getBase() const { return m_base; }
    uint64 getSize() const { return m_size; }

    /*
     * Value to add to a guest address to get the host address (wraps modulo
     * 2^64, which is what the translated [r14 + guestVA] addressing needs).
     */
    uint64 getBias() const;

    /* Makes [va, va+len) inaccessible to read/write/toHost. */
    void forbid(uint64 va, uint64 len);

    /* True when [va, va+len) lies in the region and touches no forbidden range. */
    bool isAccessible(uint64 va, uint64 len) const;

    /* Host address of guest range [va, va+len). Throws GuestFault if invalid. */
    uint8* toHost(uint64 va, uint64 len) const;

    /* Copies guest bytes out. Throws GuestFault if the range is invalid. */
    void read(uint64 va, void* dst, uint64 len) const;

    /* Copies bytes into guest memory. Throws GuestFault if the range is invalid. */
    void write(uint64 va, const void* src, uint64 len);

    /*
     * Reads the NUL-terminated string at va. The NUL must appear within
     * maxLength bytes (not counting the NUL) and inside accessible memory,
     * otherwise GuestFault is thrown. The terminator is not part of the result.
     */
    std::string readCString(uint64 va, uint64 maxLength) const;

private:
    uint64 m_base;
    uint64 m_size;
    std::vector<uint8> m_data;
    std::vector<std::pair<uint64, uint64>> m_forbidden;
};

} // namespace rt
} // namespace reforge

#endif // __TBA_REFORGE_RT_GUESTMEMORY_H
