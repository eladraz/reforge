#ifndef __TBA_REFORGE_TRANSLATOR_H
#define __TBA_REFORGE_TRANSLATOR_H

/*
 * Translator.h
 *
 * The translation driver:
 *   load PE -> flow recovery -> [lowering] -> block linker -> object -> host link
 * Failures are ReforgeError with the exit codes of Diagnostics.h.
 */

#include "reforge/Lowerer.h"
#include "reforge/Options.h"

namespace reforge {

/*
 * Runs the pipeline. lowerer may be null while the AMD64 lowering is not wired
 * in; reaching the lowering stage without one is an RC_INTERNAL error that
 * names the missing stage.
 */
void translate(const Options& options, ILowerer* lowerer);

} // namespace reforge

#endif // __TBA_REFORGE_TRANSLATOR_H
