#ifndef __TBA_REFORGE_CONFIG_H
#define __TBA_REFORGE_CONFIG_H

/*
 * Config.h
 *
 * Source and target descriptions of a Reforge translation. Source and target
 * formats, ISAs, ABIs and OS are separate concepts: the IR connects any source
 * to any target. Only the combinations of milestone 1 are enumerated.
 */

namespace reforge {

/* Container format of the input binary. */
enum class SourceFormat { PE };

/* Instruction set of the guest code. */
enum class SourceIsa { RISCV64 };

/* Calling convention of the guest code. */
enum class SourceAbi { WIN_RV64_LP64 };

/* Container format of the produced binary. */
enum class TargetFormat { ELF };

/* Instruction set of the produced code. */
enum class TargetIsa { X86_64 };

/* Operating system the produced binary runs on. */
enum class TargetOs { Linux };

/* Calling convention of the produced code. */
enum class TargetAbi { SYSV };

/* How the translated code is turned into an object file. */
enum class ObjectWriter { ELFLIB, ASM };

inline const char* toString(SourceFormat)
{
    return "pe";
}

inline const char* toString(SourceIsa)
{
    return "riscv64";
}

inline const char* toString(SourceAbi)
{
    return "win-rv64-lp64";
}

inline const char* toString(TargetFormat)
{
    return "elf";
}

inline const char* toString(TargetIsa)
{
    return "x86_64";
}

inline const char* toString(TargetOs)
{
    return "linux";
}

inline const char* toString(TargetAbi)
{
    return "sysv";
}

inline const char* toString(ObjectWriter writer)
{
    return writer == ObjectWriter::ELFLIB ? "elflib" : "asm";
}

} // namespace reforge

#endif // __TBA_REFORGE_CONFIG_H
