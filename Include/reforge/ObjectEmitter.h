#ifndef __TBA_REFORGE_OBJECTEMITTER_H
#define __TBA_REFORGE_OBJECTEMITTER_H

/*
 * ObjectEmitter.h
 *
 * Writes the translated code and the metadata as an ELF64 x86-64 relocatable
 * object. Contents (symbols: Meta.h, section 1):
 *   .text.reforge    SHF_ALLOC|SHF_EXECINSTR (RX), align 16: reforge_text,
 *                    reforge_enter
 *   .rodata.reforge  SHF_ALLOC (R), align 8: reforge_meta
 *   .note.GNU-stack  (added by the writer) non-executable stack
 * There are no relocations. Nothing is writable and executable.
 */

#include <string>
#include <vector>
#include "xStl/types.h"
#include "reforge/Config.h"
#include "reforge/HostLinker.h"

namespace reforge {

struct ObjectContents
{
    std::vector<uint8> text;
    /* Size of the entry stub at offset 0 of text (symbol size of reforge_enter). */
    uint64 enterSize;
    std::vector<uint8> meta;
    ObjectContents() : enterSize(0) {}
};

/* Writes the object with the elf library's cElf64ObjectWriter. */
void writeElfObject(const ObjectContents& contents, const std::string& path);

/*
 * Fallback: writes <path>.S with .incbin of <path>.text.bin and <path>.meta.bin
 * and assembles it with the host assembler into path. The intermediates are
 * removed on success. Throws RC_LINK_FAILURE if the assembler fails.
 */
void writeAsmObject(const ObjectContents& contents, const std::string& path,
                    const HostLinkConfig& config);

/* Dispatches on the writer. */
void emitObject(ObjectWriter writer, const ObjectContents& contents,
                const std::string& path, const HostLinkConfig& config);

} // namespace reforge

#endif // __TBA_REFORGE_OBJECTEMITTER_H
