/*
 * GuestMemory.cpp
 *
 * Bounds-checked access to the guest region. All range arithmetic is written
 * so that it cannot overflow: a range is accepted only when its length fits in
 * the bytes that remain after its start.
 */

#include <cstring>
#include "reforge/rt/GuestMemory.h"

namespace reforge {
namespace rt {

GuestMemory::GuestMemory(uint64 base, uint64 size)
    : m_base(base), m_size(size), m_data(static_cast<std::size_t>(size), 0)
{
}

uint64 GuestMemory::getBias() const
{
    uint64 host = static_cast<uint64>(reinterpret_cast<std::uintptr_t>(m_data.data()));
    return host - m_base;
}

void GuestMemory::forbid(uint64 va, uint64 len)
{
    if (len == 0)
        return;
    m_forbidden.push_back(std::make_pair(va, len));
}

bool GuestMemory::isAccessible(uint64 va, uint64 len) const
{
    if (va < m_base)
        return false;
    uint64 offset = va - m_base;
    if (offset > m_size || len > m_size - offset)
        return false;
    for (std::size_t i = 0; i < m_forbidden.size(); i++)
    {
        uint64 start = m_forbidden[i].first;
        uint64 length = m_forbidden[i].second;
        // Ranges [va, va+len) and [start, start+length) overlap?
        bool beforeEnd = va < start + length;
        bool afterStart = start < va + len;
        if (len != 0 && beforeEnd && afterStart)
            return false;
    }
    return true;
}

uint8* GuestMemory::toHost(uint64 va, uint64 len) const
{
    if (!isAccessible(va, len))
        throw GuestFault("guest memory access of " + std::to_string(len) +
                         " bytes at " + guestHex(va) +
                         " is outside guest memory [" + guestHex(m_base) + ", " +
                         guestHex(m_base + m_size) + ") or in a forbidden range");
    // m_data is never resized after construction, so the address is stable.
    return const_cast<uint8*>(m_data.data()) + (va - m_base);
}

void GuestMemory::read(uint64 va, void* dst, uint64 len) const
{
    if (len == 0)
        return;
    std::memcpy(dst, toHost(va, len), static_cast<std::size_t>(len));
}

void GuestMemory::write(uint64 va, const void* src, uint64 len)
{
    if (len == 0)
        return;
    std::memcpy(toHost(va, len), src, static_cast<std::size_t>(len));
}

std::string GuestMemory::readCString(uint64 va, uint64 maxLength) const
{
    std::string result;
    for (uint64 i = 0; i <= maxLength; i++)
    {
        if (!isAccessible(va + i, 1))
        {
            if (i == 0)
                toHost(va, 1);  // throws "... outside guest memory ..."
            throw GuestFault("unterminated guest string at " + guestHex(va) +
                             ": reached inaccessible guest memory at " +
                             guestHex(va + i));
        }
        char c = static_cast<char>(*toHost(va + i, 1));
        if (c == '\0')
            return result;
        if (i == maxLength)
            break;
        result.push_back(c);
    }
    throw GuestFault("unterminated guest string at " + guestHex(va) +
                     ": no NUL within " + std::to_string(maxLength) + " bytes");
}

} // namespace rt
} // namespace reforge
