# Reforge: local implementation agent prompt

Your goal is to build **Reforge**, an end-to-end binary recompilation tool and integration test for **dismount's semantic lifter**. Work on the local machine until the first real cross-architecture, cross-platform translation runs successfully.

## 1. Required outcome

Generate a minimal **Windows-format RISC-V64 console executable (PE32+)** that prints hello-world\n. Feed only that executable and explicit target/runtime configuration into Reforge. Reforge must translate its machine code through dismount's semantic IR and produce an **x86-64 Linux ELF executable** that runs directly on a native x86-64 Linux kernel, prints exactly those bytes to stdout, and exits successfully.

The input and output must be actual binary files. The output must contain translated x86-64 machine code derived from the RISC-V instructions. All guest instructions executed by this test must pass through dismount's semantic lifter and native code generation. A semantic interpreter can support development and testing, but cannot be the execution engine for the final acceptance test.

Reforge's wider purpose is to accept an executable for one machine/platform and generate an executable for the same or another machine/platform. Same-ISA translation is also valuable because it exercises lifting and regeneration. The first mandatory milestone is specifically RISC-V PE to native Linux x86-64 ELF.

Do not stop after writing a design, producing compilable code, dumping semantic IR, generating assembly, or creating an executable that has not been run. Continue through implementation, debugging, and verification until the acceptance criteria below pass. If a genuine external blocker cannot be resolved with the available permissions or environment, preserve your work and report the precise blocker and the last verified result. Never claim success without execution evidence.

## 2. Existing repositories and ownership

Use these existing libraries:

- https://github.com/eladraz/xStl: portable C++ infrastructure, containers, streams, memory access, and host OS abstractions.
- https://github.com/eladraz/dismount: assembly/disassembly, instruction semantics, semantic IR, and related translation interfaces.
- https://github.com/eladraz/elf: ELF structures, parsing, construction, symbols, relocations, and related functionality.
- https://github.com/eladraz/pe: PE structures and parsing infrastructure.

First inspect the local workspace, relevant AGENTS.md files, repository status, default branches, build instructions, and current code. Reuse existing local checkouts. Clone missing dependencies into a sensible sibling workspace if authorized access is available. Create the local Reforge project if it does not exist. Do not create remote repositories, publish, or push unless separately instructed.

Preserve unrelated local modifications. Use an isolated branch or worktree where appropriate. Make the necessary local changes across Reforge and these libraries; do not repeatedly ask for permission for routine implementation decisions. Keep an inventory of changed repositories and files.

The following dismount files existed during preliminary review; inspect their current implementations rather than assuming their presence means every backend is complete:

- Include/dismount/semantics/SemanticBlock.h
- Include/dismount/semantics/SemanticLifter.h
- Include/dismount/semantics/SemanticLegalizer.h
- Include/dismount/semantics/SemanticLowering.h
- Include/dismount/semantics/ArchitectureDescription.h
- RISC-V semantic lifters and architecture descriptions under proc/riscv.
- IA-32/AMD64 architecture and semantic support under proc/ia32.
- Assembly interfaces, binary dependencies, and first/second-pass binary facilities.
- FlowMapper.h and its documented limitations concerning indirect control flow.

The reviewed ELF code used ELF32 structures in important places. Verify ELF64 support and implement the required x86-64 executable support rather than assuming it already works. Likewise, verify AMD64 instruction encoding support. Extend the existing abstractions where needed.

Keep reusable instruction semantics and code generation in dismount; executable-format logic in elf/pe; portable host I/O in xStl; and translation orchestration, import adaptation, and application policy in Reforge. Avoid parallel implementations of functionality the libraries already provide.

## 3. What Reforge must do

Build a real, extensible translation pipeline:

**Load the source binary.** Validate PE headers, machine type, pointer width, subsystem, sections, entry point, image addresses, imports, and relocations used by the fixture. Distinguish file offsets, RVAs, and virtual addresses.
**Recover executable code.** Start at the entry point and follow the control flow needed by the program. Use metadata where available. Reject unresolved control flow explicitly rather than silently losing code.
**Decode and lift.** Use dismount to decode the RISC-V instructions and lift their behavior into semantic IR. Preserve source-address attribution and instruction boundaries for diagnostics.
**Legalize and generate native code.** Implement or complete the dismount functionality required to lower this IR into AMD64 instructions. Handle guest register state, temporaries, flags/comparisons where applicable, widths, memory accesses, branches, and calls correctly. Use dismount's assembler/encoding facilities, extending them if necessary.
**Adapt imported APIs and OS interactions.** Resolve recognized guest imports through Reforge adapters backed by xStl's host abstractions. Translate guest arguments, pointers, results, errors, and control flow at that boundary.
**Build the target image.** Lay out translated code, source data, guest state where needed, import bridges, and startup/runtime support. Resolve translated branches and guest-address references. Produce a valid x86-64 Linux ELF executable.
**Execute and verify.** Run the resulting ELF directly on native x86-64 Linux and check its output and exit status.
The semantic IR should allow each supported source architecture to connect to each supported target without requiring a new pairwise instruction translator. Source/target formats, ISAs, and platform ABIs must remain separate concepts in configuration and implementation.

## 4. Cross-platform calls through xStl

Reforge also translates platform dependencies into xStl-backed operations. This includes both imported library/API calls and actual guest syscall instructions when supported. printf is a library call, not a kernel syscall; model these boundaries accurately.

For the first milestone:

- The source must genuinely execute a RISC-V call to a PE import representing printf, or an explicitly documented equivalent console API. Prefer printf for this fixture.
- Resolve that import by module and symbol through a reusable import-adapter mechanism. Do not identify the whole application by filename, checksum, string contents, or instruction pattern.
- Implement the required guest-to-host calling-convention bridge. Read the guest string from its translated/mapped data, validate guest memory accesses, and return the correct result to the guest state.
- Route the console output through an appropriate xStl abstraction. Inspect the actual API. If stdout support is missing, add a small portable xStl API with a host-specific implementation and meaningful tests, then use it from the adapter. Do not invent a nonexistent xStl method in the implementation or documentation.
- A precisely documented subset of printf is sufficient initially, such as a literal string or %s; reject unsupported formats explicitly. Do not claim full variadic C runtime compatibility.
- If the fixture imports ExitProcess or an equivalent termination API, adapt it into Reforge runtime termination and propagate the requested exit status to the native program. Host startup and shutdown may use the normal host runtime.
- Do not copy Windows syscalls into the Linux executable or scatter raw Linux syscalls throughout Reforge adapters. Host-specific I/O belongs behind xStl. Normal host C/C++ runtime dependencies are acceptable; a freestanding or fully static output is not required.

Design the adapter mechanism to accommodate future file operations such as open/read/write/close with explicit guest-handle mapping. You do not need to implement a complete Windows CRT, Win32 API, or file API for the first console milestone.

## 5. Generate the Windows RISC-V fixture reproducibly

Do not assume a standard supported Windows-on-RISC-V OS or complete Microsoft CRT/toolchain exists. The required input is a **well-formed Windows PE console image containing real RISC-V64 machine code**, with an explicitly documented guest ABI/import contract understood by Reforge. Native execution of the source on Windows is not an acceptance condition. Make this distinction clear in the fixture documentation.

Use the standard PE RISC-V64 machine identifier (IMAGE_FILE_MACHINE_RISCV64, verify its defined value), PE32+ optional header, Windows console subsystem, appropriate alignment, a valid entry point, real code/data sections, and valid import metadata. Prefer a small RV64 integer instruction subset without compressed/vector instructions for the first test. Record the exact ISA extensions and calling convention used.

Investigate available clang/LLVM, binutils, and dismount facilities. Prefer a normal reproducible source/assembly build where the toolchain supports the required target. If RISC-V COFF/PE linking is unavailable, generate RISC-V code using a supported assembler and package it with a small, deterministic PE fixture builder using the project's format infrastructure. Resolve any intermediate relocations correctly. Explain this approach rather than relabeling another format as PE.

The fixture must contain:

- An entry function that prepares arguments and makes a real call through its import mechanism.
- hello-world\n in guest data, referenced by guest machine code.
- Correct stack/register behavior for the documented guest ABI.
- A defined termination path, preferably with an explicit exit status.

Keep the fixture source, builder, and build command in the repository. The acceptance run must translate an input built before invoking Reforge. The translator must operate without access to the fixture's original source code. Debug metadata can aid diagnostics but must not substitute for decoding and lifting the actual instructions.

## 6. Native output and runtime

Generate a real x86-64 Linux ELF containing translated native code. You may use the host compiler/linker to build and link a small xStl-backed runtime and startup adapter with the generated code. This is a practical way to obtain valid ELF64 startup, runtime linkage, and platform metadata. Integrate the project's ELF library where appropriate and extend it when necessary; document exactly which image-writing tasks the library and external linker perform.

The translator must not regenerate the guest application as C and compile it as a substitute for testing dismount's native lowering. Compiling host runtime helpers is allowed. Exporting translated code as assembly or an object file and assembling/linking it as part of the Reforge command is allowed, provided instruction selection and translation genuinely come from the semantic IR and dismount lowering.

Pay particular attention to:

- Preserving guest-visible register widths and the RISC-V zero register.
- Guest versus host address spaces and pointer translation, including pointers passed to imports.
- Guest return addresses and indirect calls, including RISC-V jal/jalr behavior and import thunks.
- Mapping guest memory/data pointers separately from guest code addresses.
- Distinguishing a translated guest function address from a host function pointer.
- Host SysV AMD64 stack alignment, callee-saved registers, and runtime calls.
- Guest endianness, data widths, image base, and PC-relative addressing.
- Avoiding executing guest data as code and applying sensible target section permissions.

An initial explicit guest-state structure and guest-memory mapping are acceptable. Do not optimize away the architecture boundary before the translation is correct.

## 7. Implementation workflow

Inspect the workspace and tools; build the relevant existing libraries and run their appropriate baseline tests. Record existing failures separately.
Write a short implementation plan and capability matrix based on the current code. Then implement it in the same session; the plan is not the deliverable.
Create the smallest well-formed RISC-V PE fixture and verify its headers, imports, data, entry point, and disassembly.
Build a focused decode/lift/legalize/lower test for the instruction sequence it uses. Fill gaps in dismount with reusable implementations and regression tests.
Implement the Reforge image loader, translation driver, address resolution, import adapters, and native output path.
Translate the fixture, execute the output on native x86-64 Linux, diagnose failures, and repeat until it works.
Add the integration and anti-shortcut checks below. Run affected library tests and a clean, reproducible end-to-end build.
Update documentation and provide the final verified result.
Prefer a straightforward C++ CLI using the repositories' existing conventions and CMake. Do not introduce an LLVM/QEMU/Remill lifter to replace dismount. Development tools such as cross assemblers, linkers, disassemblers, debuggers, and independent execution oracles are fine.

Keep diagnostic logs separate from program stdout. Provide optional dumps of source disassembly, lifted IR, legalized IR, generated target code, import mappings, and address mappings where practical. Errors should identify the guest address and unsupported instruction or import.

Keep useful checkpoints in local files, including commands, decisions, and remaining failures, so the work can continue after context compaction. Give brief progress updates during sustained work. Continue autonomously with reasonable implementation choices.

## 8. Acceptance criteria and meaningful verification

Provide one documented command or script that, from a clean build directory, builds dependencies, generates the input, runs Reforge, verifies the output, and executes it. A suggested interface is:

reforge --input hello-riscv64-windows.exe \
        --target-arch x86_64 --target-os linux --output-format elf \
        --output hello-x86_64-linux
./hello-x86_64-linux

You may choose equivalent option names, but document the real interface. Reforge must detect source properties from the binary rather than trusting the filename. Any fixture-specific ABI selection must be explicit and documented.

All of these must pass:

**Source identity:** independent inspection or structural validation confirms PE32+, RISC-V64, console subsystem, valid entry point, and expected imports. Show a disassembly proving real RISC-V instructions are present.
**Translation provenance:** retain diagnostics showing those guest instructions were decoded and lifted by dismount, then generated as AMD64 code. Confirm no unresolved/undefined semantic operations remain on the executed path.
**Target identity:** inspection confirms a valid x86-64 Linux ELF and AMD64 code at the translated execution path.
**Native execution:** run on native x86-64 Linux without QEMU, Wine, a RISC-V interpreter, binfmt emulation, or another guest execution engine. An ordinary linked host runtime is allowed. Verify the environment; an emulated x86-64 container on an ARM host does not count as native execution. If needed, use an available authorized native x86-64 Linux machine for the final run.
**Exact behavior:** stdout is exactly hello-world\n, stderr is empty for normal execution, and the exit status is zero. Verify byte contents, not merely a matching substring.
**xStl path:** a trace or test proves the imported output call reaches the xStl-backed adapter. Normal output must not include tracing. The adapter reads the guest-provided data and produces the appropriate return value.
**Data variation:** generate a second input with a different message and demonstrate the corresponding translated output. The translator and runtime must contain no hardcoded hello-world response.
**Semantic variation:** generate a small companion fixture that computes or branches on a value in RISC-V machine code and uses the result in its output or exit status. Change that value and demonstrate a changed result after translation. This must exercise the lifter and native lowering, not only the import adapter.
**Failure behavior:** unsupported instructions, unresolved imports, and malformed/truncated inputs produce a clear failure and nonzero status; they must not produce an apparently successful executable.
**Regression and reproducibility:** affected library tests and the automated end-to-end test pass. Re-run the documented build/run path from a clean build directory. Record toolchain versions and dependency revisions.

Do not satisfy the task by compiling the original hello-world source directly for Linux, emitting a hardcoded print program, extracting the string and printing it, copying a prebuilt ELF, renaming a file, or running the source under an emulator. Every guest instruction needed to produce the observed behavior must be accounted for through semantic translation. Recognized external calls may be replaced at the explicit import/API boundary.

## 9. Scope and final deliverables

Prioritize one correct RISC-V PE to AMD64 Linux ELF path. Design extension points for other ISAs, ELF/PE/Mach-O formats, same-ISA regeneration, and additional xStl-backed APIs, but do not delay the first working result to implement every combination. Do not advertise combinations that have not been implemented and tested.

Deliver:

- Working Reforge sources and the necessary changes to xStl, dismount, elf, and pe.
- Reproducible fixture sources/builders and the generated source PE.
- The translated x86-64 Linux ELF and its required runtime dependencies.
- A single automated build/translation/execution test entry point.
- Focused regression tests for new semantic/code-generation and adapter behavior.
- A README explaining architecture, prerequisites, build/run commands, the fixture ABI, current support, and limitations.
- A concise final report with actual commands, input/output inspection, native execution evidence, exact stdout bytes, exit status, tests run, dependency revisions, and changed repository paths.

Do the work, run the result, and continue fixing problems until the specified end-to-end acceptance criteria pass.
