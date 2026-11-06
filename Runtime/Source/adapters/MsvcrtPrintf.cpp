/*
 * MsvcrtPrintf.cpp
 *
 * msvcrt.dll!printf for the guest LP64 convention: a0 is the format, a1..a7
 * are the variadic arguments. The whole output is formatted before anything is
 * written, so a rejected conversion never leaves a partial line on stdout.
 */

#include <string>
#include "reforge/rt/adapters/MsvcrtPrintf.h"

namespace reforge {
namespace rt {

std::string formatPrintf(const GuestMemory& memory, uint64 fmtVA,
                         const uint64* args, std::size_t argCount,
                         uint64 callSite)
{
    std::string format = memory.readCString(fmtVA, PRINTF_MAX_FORMAT);
    std::string out;
    std::size_t nextArg = 0;

    for (std::size_t i = 0; i < format.size(); i++)
    {
        char c = format[i];
        if (c != '%')
        {
            out.push_back(c);
            continue;
        }

        if (i + 1 >= format.size())
            throw GuestFault("incomplete printf conversion at the end of the format " +
                             guestHex(fmtVA) + " (guest call " + guestHex(callSite) + ")");
        char conversion = format[++i];

        if (conversion == '%')
        {
            out.push_back('%');
        }
        else if (conversion == 's')
        {
            if (nextArg >= argCount)
                throw GuestFault("printf %s without an argument (guest call " +
                                 guestHex(callSite) + ")");
            out += memory.readCString(args[nextArg++], PRINTF_MAX_STRING);
        }
        else
        {
            throw GuestFault(std::string("unsupported printf conversion '%") +
                             conversion + "' at guest " + guestHex(callSite));
        }
    }
    return out;
}

void msvcrtPrintfAdapter(AdapterContext& context)
{
    uint64 args[7];
    for (unsigned i = 0; i < 7; i++)
        args[i] = context.arg(i + 1);

    std::string text = formatPrintf(context.memory, context.arg(0), args, 7,
                                    context.callSite);
    context.output.writeStdout(text.data(), text.size());
    context.trace("printf -> xStl (" + std::to_string(text.size()) +
                  " bytes to standard output)");
    context.setResult(text.size());
}

} // namespace rt
} // namespace reforge
