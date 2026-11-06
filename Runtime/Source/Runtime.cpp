/*
 * Runtime.cpp
 *
 * The dispatch loop. Translated code runs in reforge_enter(); it returns to
 * this loop whenever it needs the host: a dynamic control transfer (the next
 * guest pc goes through the block table) or a trap. Calls to import thunks are
 * handled here by calling the adapter; the guest return address is in ra.
 */

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <unistd.h>
#include "reforge/GuestState.h"
#include "reforge/Meta.h"
#include "reforge/rt/AdapterRegistry.h"
#include "reforge/rt/GuestMemory.h"
#include "reforge/rt/Program.h"
#include "reforge/rt/Runtime.h"

// Entry stub of the translated object (see Include/reforge/Meta.h, section 1).
extern "C" void reforge_enter(reforge::GuestState* state, const void* hostTarget);

namespace reforge {
namespace rt {

namespace {

/*
 * Translated loads and stores address host memory as [bias + guest address]
 * without a range check (a check per access would cost an extra compare and
 * branch on every memory operation). A guest address outside the guest region
 * therefore faults on the host, or, if the host address happens to be mapped,
 * reads or writes host memory: the guest is trusted not to do that. The handler
 * below only turns the common unmapped-address case into a diagnostic and the
 * runtime fault exit status instead of a silent SIGSEGV.
 *
 * The handler runs in signal context, so it uses only async-signal-safe
 * operations: it formats into a stack buffer (formatFaultLine, no allocation,
 * no locks) and calls write(2) and _exit(2). The xStl streams are not used
 * there. xStl has no signal abstraction (SPEC 4 keeps host I/O behind xStl),
 * which is why the sigaction/sigaltstack calls below are the one place where
 * the runtime uses POSIX directly, and only on this crash path.
 */
volatile uint64 g_faultBias = 0;

void writeFully(int fd, const char* data, std::size_t length)
{
    while (length > 0)
    {
        ssize_t written = write(fd, data, length);
        if (written < 0)
        {
            if (errno == EINTR)
                continue;
            return;
        }
        data += written;
        length -= static_cast<std::size_t>(written);
    }
}

void guestFaultHandler(int signal, siginfo_t* info, void*)
{
    const uint64 host = static_cast<uint64>(reinterpret_cast<std::uintptr_t>(info->si_addr));
    char line[256];
    std::size_t length = formatFaultLine(line, sizeof(line), signal, host, g_faultBias);
    writeFully(STDERR_FILENO, line, length);
    _exit(REFORGE_RUNTIME_FAULT_EXIT);
}

// The only platform-specific part of the runtime: the fault handler and its
// alternate stack (a stack overflow must still reach the handler).
void installFaultHandler(uint64 bias)
{
    g_faultBias = bias;
    static char alternateStack[64 * 1024];
    stack_t stack;
    std::memset(&stack, 0, sizeof(stack));
    stack.ss_sp = alternateStack;
    stack.ss_size = sizeof(alternateStack);
    sigaltstack(&stack, nullptr);
    struct sigaction action;
    std::memset(&action, 0, sizeof(action));
    action.sa_sigaction = &guestFaultHandler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, nullptr);
    sigaction(SIGBUS, &action, nullptr);
}

} // anonymous namespace

int runGuestProgram(const uint8* meta, uint64 metaSize, const uint8* text,
                    OutputSink& output, bool trace)
{
    Program program = Program::parse(meta, metaSize);

    GuestMemory memory(program.guestBase, program.regionSize);
    memory.write(program.guestBase, program.image, program.sizeOfImage);
    // The thunks are call targets only; data accesses to them are invalid.
    memory.forbid(program.thunkVA, program.getThunkAreaSize());

    GuestState state;
    std::memset(&state, 0, sizeof(state));
    state.pc = program.entryVA;
    state.x[1] = REFORGE_HOST_RETURN_PC;
    state.x[2] = program.stackTopVA;
    state.bias = memory.getBias();
    installFaultHandler(state.bias);

    for (;;)
    {
        if (state.pc == REFORGE_HOST_RETURN_PC)
            return static_cast<int>(state.x[10] & 0xff);

        uint64 importIndex = 0;
        if (program.lookupThunk(state.pc, importIndex))
        {
            const ProgramImport& import = program.imports[static_cast<std::size_t>(importIndex)];
            AdapterFn adapter = lookupAdapter(import.adapter);
            if (adapter == nullptr)
                throw GuestFault("no adapter for " + import.module + "!" +
                                 import.symbol);

            // The jalr that called the thunk is 4 bytes before the return address.
            uint64 callSite = state.x[1] - 4;
            if (trace)
            {
                std::string line = "reforge: trace: import " + import.module + "!" +
                                   import.symbol + " at guest " + guestHex(callSite) + "\n";
                output.writeStderr(line.data(), line.size());
            }

            AdapterContext context(state, memory, output, trace, import, callSite);
            adapter(context);
            if (context.isTerminated())
                return static_cast<int>(context.getExitStatus() & 0xff);

            state.pc = state.x[1];
            continue;
        }

        uint64 hostOffset = 0;
        if (!program.lookupBlock(state.pc, hostOffset))
            throw GuestFault("unresolved guest pc " + guestHex(state.pc) +
                             ": not the start of a recovered block");

        state.exitReason = 0;
        reforge_enter(&state, text + hostOffset);

        const uint64 reason = state.exitReason & REFORGE_EXIT_REASON_MASK;
        if (reason == REFORGE_EXIT_DISPATCH || reason == REFORGE_EXIT_CALL ||
            reason == REFORGE_EXIT_RETURN)
            continue;
        if (reason == REFORGE_EXIT_TRAP)
            throw GuestFault("guest trap at " + guestHex(state.pc) +
                             " (kind/number " + guestHex(state.exitReason >> 8) + ")");
        throw GuestFault("translated code returned to the runtime without an exit reason "
                         "(pc " + guestHex(state.pc) + ")");
    }
}

} // namespace rt
} // namespace reforge
