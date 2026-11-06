/*
 * Options.cpp
 *
 * Command line parsing of the reforge driver. Accepts "--name value" and
 * "--name=value". The raw values are collected first and validated together
 * at the end, so the rest of the driver only sees supported source and target
 * combinations.
 */
#include <map>
#include <set>
#include "reforge/Diagnostics.h"
#include "reforge/Options.h"

namespace reforge {

namespace {

/* Options that take one value. */
const char* const VALUE_OPTIONS[] = {
    "input", "output", "emit-object", "dump-dir",
    "source-format", "source-arch", "target-arch", "target-os",
    "output-format", "object-writer",
};

/* Options that are switches. */
const char* const FLAG_OPTIONS[] = {
    "help", "verbose", "no-link",
    "dump-disasm", "dump-ir", "dump-legalized", "dump-target",
    "dump-imports", "dump-addrmap",
};

template <size_t N>
bool isOneOf(const std::string& name, const char* const (&list)[N])
{
    for (size_t i = 0; i < N; i++)
        if (name == list[i])
            return true;
    return false;
}

[[noreturn]] void unsupportedValue(const std::string& option,
                                   const std::string& value,
                                   const char* supported)
{
    throw ReforgeError(RC_USAGE, "unsupported value '" + value + "' for --" +
                       option + " (supported: " + supported + ")");
}

} // anonymous namespace

Options parseOptions(int argc, const char* const* argv)
{
    std::map<std::string, std::string> values;
    std::set<std::string> flags;

    for (int i = 1; i < argc; i++)
    {
        std::string arg = argv[i];
        if (arg.size() < 3 || arg.compare(0, 2, "--") != 0)
            throw ReforgeError(RC_USAGE, "unexpected argument '" + arg + "'");

        std::string name = arg.substr(2);
        std::string value;
        bool hasValue = false;
        size_t eq = name.find('=');
        if (eq != std::string::npos)
        {
            value = name.substr(eq + 1);
            name = name.substr(0, eq);
            hasValue = true;
        }

        if (isOneOf(name, FLAG_OPTIONS))
        {
            if (hasValue)
                throw ReforgeError(RC_USAGE, "option --" + name + " takes no value");
            flags.insert(name);
        }
        else if (isOneOf(name, VALUE_OPTIONS))
        {
            if (!hasValue)
            {
                if (i + 1 >= argc)
                    throw ReforgeError(RC_USAGE, "missing value for --" + name);
                value = argv[++i];
            }
            values[name] = value;
        }
        else
        {
            throw ReforgeError(RC_USAGE, "unknown option '" + arg + "'");
        }
    }

    Options options;
    options.helpRequested = flags.count("help") != 0;
    options.verbose = flags.count("verbose") != 0;
    options.noLink = flags.count("no-link") != 0;
    options.dumpDisasm = flags.count("dump-disasm") != 0;
    options.dumpIr = flags.count("dump-ir") != 0;
    options.dumpLegalized = flags.count("dump-legalized") != 0;
    options.dumpTarget = flags.count("dump-target") != 0;
    options.dumpImports = flags.count("dump-imports") != 0;
    options.dumpAddrmap = flags.count("dump-addrmap") != 0;

    options.input = values["input"];
    options.output = values["output"];
    options.emitObject = values["emit-object"];
    options.dumpDir = values["dump-dir"];

    // The source and target enums hold the only values of milestone 1; the
    // strings given on the command line are checked against them below.
    options.sourceFormat = SourceFormat::PE;
    options.sourceIsa = SourceIsa::RISCV64;
    options.sourceAbi = SourceAbi::WIN_RV64_LP64;
    options.targetFormat = TargetFormat::ELF;
    options.targetIsa = TargetIsa::X86_64;
    options.targetOs = TargetOs::Linux;
    options.targetAbi = TargetAbi::SYSV;
    options.objectWriter = ObjectWriter::ELFLIB;

    if (options.helpRequested)
        return options;

    // Anything other than the defaults is rejected with a usage error.
    if (values.count("source-format") && values["source-format"] != "pe")
        unsupportedValue("source-format", values["source-format"], "pe");
    if (values.count("source-arch") && values["source-arch"] != "riscv64")
        unsupportedValue("source-arch", values["source-arch"], "riscv64");
    if (values.count("output-format") && values["output-format"] != "elf")
        unsupportedValue("output-format", values["output-format"], "elf");
    if (values.count("target-arch") && values["target-arch"] != "x86_64")
        unsupportedValue("target-arch", values["target-arch"], "x86_64");
    if (values.count("target-os") && values["target-os"] != "linux")
        unsupportedValue("target-os", values["target-os"], "linux");
    if (values.count("object-writer"))
    {
        const std::string& writer = values["object-writer"];
        if (writer == "elflib")
            options.objectWriter = ObjectWriter::ELFLIB;
        else if (writer == "asm")
            options.objectWriter = ObjectWriter::ASM;
        else
            unsupportedValue("object-writer", writer, "elflib, asm");
    }

    if (options.input.empty())
        throw ReforgeError(RC_USAGE, "missing required option --input");
    if (options.output.empty() && !options.noLink)
        throw ReforgeError(RC_USAGE, "missing required option --output");

    return options;
}

void printUsage(std::ostream& out)
{
    out << "Usage: reforge --input <pe> --output <exe> [options]\n"
           "\n"
           "Translates a Windows-format PE32+ RISC-V64 console program into an\n"
           "x86-64 Linux ELF executable.\n"
           "\n"
           "Input and output:\n"
           "  --input <file>            input PE32+ image (RISCV64, console)\n"
           "  --output <file>           output executable (not needed with --no-link)\n"
           "  --emit-object <file.o>    also keep the generated object file\n"
           "  --no-link                 stop after writing the object file\n"
           "\n"
           "Source and target (defaults shown; only these values are supported):\n"
           "  --source-format pe        --source-arch riscv64\n"
           "  --target-arch x86_64      --target-os linux\n"
           "  --output-format elf\n"
           "  --object-writer elflib|asm   object writer (default elflib)\n"
           "\n"
           "Diagnostics (written to stderr, never to stdout):\n"
           "  --dump-disasm --dump-ir --dump-legalized --dump-target\n"
           "  --dump-imports --dump-addrmap\n"
           "  --dump-dir <dir>          write the dumps into <dir> (default: stderr)\n"
           "  --verbose                 print the resolved configuration\n"
           "  --help                    show this help and exit\n";
}

} // namespace reforge
