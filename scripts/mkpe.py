#!/usr/bin/env python3
"""Deterministic builder for Reforge RISC-V64 console PE32+ fixtures.

Pipeline (all steps deterministic, no timestamps):
  1. Read the guest source (RV64I assembly) and its `# @import MODULE!SYMBOL`
     directives. Every `__imp_SYMBOL` referenced by the source must be declared,
     and every declaration must be referenced.
  2. Write `defs.inc` (integer `.set` defines) and `strings.inc` (`.asciz`
     string labels from --string), both included by the source.
  3. Generate `idata.S`: import descriptors, ILT/IAT, hint/name entries and DLL
     names, at the fixed RVA of the .idata section. `__imp_SYMBOL` labels mark
     the IAT slots.
  4. Generate `image.lds`: fixed layout, ImageBase 0x140000000,
     .text RVA 0x1000, .rdata 0x2000, .data 0x3000, .idata 0x4000.
  5. Assemble with llvm-mc (riscv64, -c and -relax disabled, so every guest
     instruction is a 4-byte RV64I encoding) and link with ld.lld --no-relax.
  6. Parse the linked ELF64 itself (no pyelftools) and emit a PE32+ image
     (Machine 0x5064, Magic 0x20b, console subsystem, RELOCS_STRIPPED).

Stdlib only. Usage:
  mkpe.py [--define NAME=INT]... [--string NAME=TEXT]... \
          [--workdir DIR] [--elf-out PATH] SOURCE.S -o OUT.exe

--string TEXT accepts the C escapes \\n \\t \\r \\0 \\\\ \\" \\' and \\xNN (others are
rejected); it is emitted as a NUL-terminated `.asciz` label.
"""

import argparse
import codecs
import os
import re
import struct
import subprocess
import sys

IMAGE_BASE = 0x140000000
FILE_ALIGN = 0x200
SECTION_ALIGN = 0x1000
HEADERS_SIZE = 0x400
PE_OFFSET = 0x80  # e_lfanew

MACHINE_RISCV64 = 0x5064
IMAGE_FILE_RELOCS_STRIPPED = 0x0001
IMAGE_FILE_EXECUTABLE_IMAGE = 0x0002
IMAGE_FILE_LARGE_ADDRESS_AWARE = 0x0020
IMAGE_SUBSYSTEM_WINDOWS_CUI = 3
IMAGE_DLLCHARACTERISTICS_NX_COMPAT = 0x0100
PE32PLUS_MAGIC = 0x20B

SCN_CODE = 0x00000020
SCN_INIT_DATA = 0x00000040
SCN_MEM_EXECUTE = 0x20000000
SCN_MEM_READ = 0x40000000
SCN_MEM_WRITE = 0x80000000

# name -> (fixed RVA, characteristics, is_code)
SECTION_LAYOUT = {
    ".text": (0x1000, SCN_CODE | SCN_MEM_EXECUTE | SCN_MEM_READ, True),
    ".rdata": (0x2000, SCN_INIT_DATA | SCN_MEM_READ, False),
    ".data": (0x3000, SCN_INIT_DATA | SCN_MEM_READ | SCN_MEM_WRITE, False),
    ".idata": (0x4000, SCN_INIT_DATA | SCN_MEM_READ | SCN_MEM_WRITE, False),
}
IDATA_RVA = SECTION_LAYOUT[".idata"][0]

EM_RISCV = 243
ET_EXEC = 2
SHT_NOBITS = 8
SHF_ALLOC = 0x2


class BuildError(Exception):
    pass


def align_up(value, alignment):
    return (value + alignment - 1) // alignment * alignment


def run(cmd):
    try:
        subprocess.check_call(cmd)
    except (OSError, subprocess.CalledProcessError) as exc:
        raise BuildError("command failed: %s (%s)" % (" ".join(cmd), exc))


# ---------------------------------------------------------------------------
# Source scanning
# ---------------------------------------------------------------------------

IMPORT_DIRECTIVE = re.compile(r"^\s*#\s*@import\s+([A-Za-z0-9_.\-]+)!([A-Za-z_][A-Za-z0-9_]*)\s*$")
IMP_REFERENCE = re.compile(r"__imp_([A-Za-z_][A-Za-z0-9_]*)")


def scan_imports(source_text):
    """Return ordered list of (module, symbol) from @import directives."""
    imports = []
    seen = set()
    for lineno, line in enumerate(source_text.splitlines(), 1):
        if "@import" not in line:
            continue
        match = IMPORT_DIRECTIVE.match(line)
        if not match:
            raise BuildError("line %d: malformed @import directive: %r" % (lineno, line.strip()))
        key = (match.group(1).lower(), match.group(2))
        if key in seen:
            raise BuildError("line %d: duplicate @import %s!%s" % (lineno, match.group(1), match.group(2)))
        seen.add(key)
        imports.append((match.group(1), match.group(2)))
    return imports


def check_import_references(source_text, imports):
    referenced = set(IMP_REFERENCE.findall(source_text))
    declared = set(sym for _, sym in imports)
    undeclared = sorted(referenced - declared)
    unused = sorted(declared - referenced)
    if undeclared:
        raise BuildError("__imp_ symbols referenced without @import: %s" % ", ".join(undeclared))
    if unused:
        raise BuildError("@import declared but never referenced: %s" % ", ".join(unused))


# ---------------------------------------------------------------------------
# Generated include files
# ---------------------------------------------------------------------------

_SIMPLE_ESCAPES = {"n": 0x0A, "t": 0x09, "r": 0x0D, "0": 0x00, "\\": 0x5C, '"': 0x22, "'": 0x27}


def decode_c_string(text):
    """Decode a command-line string value with C escapes (\\n, \\t, \\r, \\0,
    \\\\, \\", \\', \\xNN). Anything else is an error. Returns bytes."""
    out = bytearray()
    i = 0
    while i < len(text):
        ch = text[i]
        if ch != "\\":
            out.extend(ch.encode("utf-8"))
            i += 1
            continue
        if i + 1 >= len(text):
            raise BuildError("trailing backslash in string %r" % text)
        esc = text[i + 1]
        if esc == "x":
            digits = text[i + 2:i + 4]
            if len(digits) != 2 or not all(c in "0123456789abcdefABCDEF" for c in digits):
                raise BuildError("bad \\x escape in string %r" % text)
            out.append(int(digits, 16))
            i += 4
        elif esc in _SIMPLE_ESCAPES:
            out.append(_SIMPLE_ESCAPES[esc])
            i += 2
        else:
            raise BuildError("unsupported escape \\%s in string %r" % (esc, text))
    return bytes(out)


def asm_string_literal(data):
    out = []
    for byte in data:
        ch = chr(byte)
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif 0x20 <= byte < 0x7F:
            out.append(ch)
        else:
            out.append("\\%03o" % byte)
    return '"' + "".join(out) + '"'


def write_defs(path, defines):
    lines = ["# generated by scripts/mkpe.py; do not edit"]
    for name, value in defines:
        lines.append(".set %s, %d" % (name, value))
    write_text(path, "\n".join(lines) + "\n")


def write_strings(path, strings):
    lines = ["# generated by scripts/mkpe.py; do not edit"]
    for name, text in strings:
        data = decode_c_string(text)
        lines.append("%s:" % name)
        lines.append("    .asciz %s" % asm_string_literal(data))
    write_text(path, "\n".join(lines) + "\n")


def write_text(path, text):
    with open(path, "w", newline="\n") as handle:
        handle.write(text)


# ---------------------------------------------------------------------------
# Import table (.idata) layout and emission
# ---------------------------------------------------------------------------

class IdataLayout(object):
    """Fixed, deterministic layout of the import directory inside .idata.

    Order: IMAGE_IMPORT_DESCRIPTOR array, all ILTs (per module), all IATs
    (per module, contiguous so the IAT data directory covers them), hint/name
    entries, DLL name strings.
    """

    def __init__(self, imports):
        self.modules = []
        self.symbols = {}  # module -> [symbol]
        for module, symbol in imports:
            if module not in self.symbols:
                self.modules.append(module)
                self.symbols[module] = []
            self.symbols[module].append(symbol)

        n = len(self.modules)
        self.descriptor_off = 0
        off = 20 * (n + 1)

        self.ilt_off = {}
        self.iat_off = {}
        self.hn_off = {}
        self.dll_off = {}

        off = align_up(off, 8)
        for module in self.modules:
            self.ilt_off[module] = off
            off += 8 * (len(self.symbols[module]) + 1)
        self.iat_start = off
        for module in self.modules:
            self.iat_off[module] = off
            off += 8 * (len(self.symbols[module]) + 1)
        self.iat_size = off - self.iat_start

        for module in self.modules:
            for symbol in self.symbols[module]:
                off = align_up(off, 2)
                self.hn_off[(module, symbol)] = off
                off += 2 + len(symbol) + 1
        for module in self.modules:
            self.dll_off[module] = off
            off += len(module) + 1
        self.size = off

    def rva(self, offset):
        return IDATA_RVA + offset


class AsmWriter(object):
    """Emits idata.S while tracking the section offset, so that layout and
    emitted bytes cannot drift apart."""

    def __init__(self):
        self.lines = []
        self.off = 0

    def label(self, name):
        self.lines.append("    .globl %s" % name)
        self.lines.append("%s:" % name)

    def pad_to(self, target):
        if target < self.off:
            raise BuildError("internal layout error: at %d, wanted %d" % (self.off, target))
        if target > self.off:
            self.lines.append("    .zero %d" % (target - self.off))
            self.off = target

    def long(self, value):
        self.lines.append("    .long 0x%08x" % (value & 0xFFFFFFFF))
        self.off += 4

    def quad(self, value):
        self.lines.append("    .quad 0x%016x" % (value & 0xFFFFFFFFFFFFFFFF))
        self.off += 8

    def short(self, value):
        self.lines.append("    .short 0x%04x" % (value & 0xFFFF))
        self.off += 2

    def asciz(self, text):
        self.lines.append("    .asciz %s" % asm_string_literal(text.encode("ascii")))
        self.off += len(text) + 1


def write_idata(path, layout):
    w = AsmWriter()
    w.lines.append("# generated by scripts/mkpe.py: PE import directory (.idata)")
    w.lines.append("    .section .idata,\"aw\",@progbits")
    w.lines.append("    .balign 8")

    # Import descriptors (IMAGE_IMPORT_DESCRIPTOR: 5 dwords each, incl. null).
    for module in layout.modules:
        w.long(layout.rva(layout.ilt_off[module]))  # OriginalFirstThunk
        w.long(0)  # TimeDateStamp: deterministic
        w.long(0)  # ForwarderChain
        w.long(layout.rva(layout.dll_off[module]))  # Name
        w.long(layout.rva(layout.iat_off[module]))  # FirstThunk
    for _ in range(5):
        w.long(0)

    # ILT: per module, RVA of hint/name entries, then null terminator.
    for module in layout.modules:
        w.pad_to(layout.ilt_off[module])
        for symbol in layout.symbols[module]:
            w.quad(layout.rva(layout.hn_off[(module, symbol)]))
        w.quad(0)

    # IAT: same values before loading; each slot is a labelled __imp_ symbol.
    for module in layout.modules:
        w.pad_to(layout.iat_off[module])
        for symbol in layout.symbols[module]:
            w.label("__imp_%s" % symbol)
            w.quad(layout.rva(layout.hn_off[(module, symbol)]))
        w.quad(0)

    # Hint/name entries.
    for module in layout.modules:
        for symbol in layout.symbols[module]:
            w.pad_to(layout.hn_off[(module, symbol)])
            w.short(0)  # Hint
            w.asciz(symbol)

    # DLL names.
    for module in layout.modules:
        w.pad_to(layout.dll_off[module])
        w.asciz(module)

    w.pad_to(layout.size)
    write_text(path, "\n".join(w.lines) + "\n")


def write_linker_script(path):
    lines = [
        "/* generated by scripts/mkpe.py: fixed RVA layout for PE image base */",
        "ENTRY(_start)",
        "SECTIONS",
        "{",
        "  . = 0x%x;" % (IMAGE_BASE + SECTION_LAYOUT[".text"][0]),
        "  .text : { *(.text .text.*) }",
        "  . = 0x%x;" % (IMAGE_BASE + SECTION_LAYOUT[".rdata"][0]),
        "  .rdata : { *(.rdata .rdata.*) *(.rodata .rodata.*) }",
        "  . = 0x%x;" % (IMAGE_BASE + SECTION_LAYOUT[".data"][0]),
        "  .data : { *(.data .data.*) }",
        "  . = 0x%x;" % (IMAGE_BASE + SECTION_LAYOUT[".idata"][0]),
        "  .idata : { *(.idata) }",
        "  /DISCARD/ : { *(.comment) *(.riscv.attributes) *(.note*) *(.eh_frame) }",
        "}",
        "",
    ]
    write_text(path, "\n".join(lines))


# ---------------------------------------------------------------------------
# Minimal ELF64 little-endian reader (ld.lld output)
# ---------------------------------------------------------------------------

class Elf64(object):
    def __init__(self, data):
        if data[:4] != b"\x7fELF":
            raise BuildError("linked output is not an ELF file")
        if data[4] != 2 or data[5] != 1:
            raise BuildError("linked output is not ELF64 little-endian")
        e_type, e_machine = struct.unpack_from("<HH", data, 16)
        if e_machine != EM_RISCV:
            raise BuildError("linked output machine %d is not EM_RISCV" % e_machine)
        if e_type != ET_EXEC:
            raise BuildError("linked output type %d is not ET_EXEC" % e_type)
        self.entry = struct.unpack_from("<Q", data, 24)[0]
        shoff = struct.unpack_from("<Q", data, 0x28)[0]
        shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x3A)
        if shentsize != 64:
            raise BuildError("unexpected section header size %d" % shentsize)
        raw = []
        for index in range(shnum):
            fields = struct.unpack_from("<IIQQQQIIQQ", data, shoff + index * 64)
            raw.append(fields)
        strtab_hdr = raw[shstrndx]
        shstr = data[strtab_hdr[4]:strtab_hdr[4] + strtab_hdr[5]]
        self.sections = {}
        self.symbols = {}
        for index, fields in enumerate(raw):
            name_off, sh_type, flags, addr, offset, size, link, _info, _align, _entsize = fields
            name = _cstr(shstr, name_off)
            self.sections[name] = {
                "type": sh_type,
                "flags": flags,
                "addr": addr,
                "offset": offset,
                "size": size,
                "index": index,
            }
            if sh_type == 2:  # SHT_SYMTAB
                strs = raw[link]
                strdata = data[strs[4]:strs[4] + strs[5]]
                for sym_off in range(0, size, 24):
                    st_name, _info, _other, _shndx, st_value, _st_size = struct.unpack_from(
                        "<IBBHQQ", data, offset + sym_off)
                    if st_name:
                        self.symbols[_cstr(strdata, st_name)] = st_value
        self._data = data

    def section_bytes(self, name):
        sec = self.sections[name]
        if sec["type"] == SHT_NOBITS:
            return b"\x00" * sec["size"]
        return self._data[sec["offset"]:sec["offset"] + sec["size"]]


def _cstr(blob, offset):
    end = blob.index(b"\x00", offset)
    return blob[offset:end].decode("ascii")


# ---------------------------------------------------------------------------
# PE32+ writer
# ---------------------------------------------------------------------------

def check_rv64i_words(code):
    """Every guest instruction must be a 32-bit encoding (low bits 0b11)."""
    if len(code) % 4:
        raise BuildError(".text size %d is not a multiple of 4: compressed or relaxed code" % len(code))
    for off in range(0, len(code), 4):
        word = struct.unpack_from("<I", code, off)[0]
        if word & 0x3 != 0x3:
            raise BuildError(".text+0x%x: non-32-bit encoding 0x%08x" % (off, word))


def build_pe(elf, entry_symbol, imports_layout, idata_bytes, raw_words=False):
    # Any allocated content outside the four known sections (for example
    # .bss or a stray orphan section) would be silently dropped from the image.
    for name, sec in sorted(elf.sections.items()):
        if (sec["flags"] & SHF_ALLOC) and sec["size"] and name not in SECTION_LAYOUT:
            raise BuildError("linked image has unsupported allocated section %s" % name)

    sections = []  # (name, rva, vsize, raw bytes, characteristics)
    for name in (".text", ".rdata", ".data", ".idata"):
        if name not in elf.sections:
            continue
        rva_expected, chars, _is_code = SECTION_LAYOUT[name]
        sec = elf.sections[name]
        if sec["size"] == 0:
            continue  # an empty section would be a zero-size PE section
        rva = sec["addr"] - IMAGE_BASE
        if rva != rva_expected:
            raise BuildError("%s placed at RVA 0x%x, expected 0x%x" % (name, rva, rva_expected))
        if sec["type"] == SHT_NOBITS and sec["size"]:
            raise BuildError("%s is NOBITS; PE writer expects file-backed sections" % name)
        data = elf.section_bytes(name)
        if name == ".idata":
            if data != idata_bytes:
                raise BuildError(".idata bytes from link differ from generated layout")
        if len(data) > SECTION_ALIGN:
            raise BuildError("%s is larger than one section slot (0x%x)" % (name, SECTION_ALIGN))
        sections.append((name, rva, len(data), data, chars))

    if not sections or sections[0][0] != ".text":
        raise BuildError("no .text section in linked image")

    text = [s for s in sections if s[0] == ".text"][0]
    if not raw_words:
        check_rv64i_words(text[3])

    entry_va = elf.symbols.get(entry_symbol)
    if entry_va is None:
        raise BuildError("entry symbol %s not found in linked image" % entry_symbol)
    entry_rva = entry_va - IMAGE_BASE
    if not (text[1] <= entry_rva < text[1] + text[2]):
        raise BuildError("entry RVA 0x%x is outside .text" % entry_rva)
    if elf.entry != entry_va:
        raise BuildError("ELF e_entry 0x%x != %s 0x%x" % (elf.entry, entry_symbol, entry_va))

    # Raw layout: each section starts at a FileAlignment boundary after headers.
    raw_off = HEADERS_SIZE
    table = []
    for name, rva, vsize, data, chars in sections:
        raw_size = align_up(len(data), FILE_ALIGN)
        table.append((name, rva, vsize, raw_off, raw_size, chars, data))
        raw_off += raw_size
    file_size = raw_off

    size_of_code = sum(t[4] for t in table if t[5] & SCN_CODE)
    size_of_init = sum(t[4] for t in table if t[5] & SCN_INIT_DATA)
    size_of_image = align_up(max(t[1] + align_up(t[2], SECTION_ALIGN) for t in table), SECTION_ALIGN)

    dirs = [(0, 0)] * 16
    dirs[1] = (IDATA_RVA + imports_layout.descriptor_off, 20 * (len(imports_layout.modules) + 1))
    dirs[12] = (IDATA_RVA + imports_layout.iat_start, imports_layout.iat_size)

    out = bytearray(HEADERS_SIZE)

    # DOS header.
    struct.pack_into("<H", out, 0, 0x5A4D)
    struct.pack_into("<I", out, 0x3C, PE_OFFSET)

    # PE signature and COFF file header.
    out[PE_OFFSET:PE_OFFSET + 4] = b"PE\x00\x00"
    coff = PE_OFFSET + 4
    struct.pack_into("<HHIIIHH", out, coff,
                     MACHINE_RISCV64, len(table), 0, 0, 0,
                     240,  # SizeOfOptionalHeader: PE32+ with 16 data directories
                     IMAGE_FILE_RELOCS_STRIPPED | IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_LARGE_ADDRESS_AWARE)

    # PE32+ optional header.
    opt = coff + 20
    fixed = struct.pack(
        "<HBBIIIIIQIIHHHHHHIIIIHHQQQQII",
        PE32PLUS_MAGIC,
        0, 0,
        size_of_code,
        size_of_init,
        0,  # SizeOfUninitializedData
        entry_rva,
        text[1],  # BaseOfCode
        IMAGE_BASE,
        SECTION_ALIGN,
        FILE_ALIGN,
        6, 0,  # OS version
        0, 0,  # image version
        6, 0,  # subsystem version
        0,  # Win32VersionValue
        size_of_image,
        HEADERS_SIZE,
        0,  # CheckSum
        IMAGE_SUBSYSTEM_WINDOWS_CUI,
        IMAGE_DLLCHARACTERISTICS_NX_COMPAT,
        0x100000, 0x1000, 0x100000, 0x1000,  # stack/heap reserve/commit
        0,  # LoaderFlags
        16,  # NumberOfRvaAndSizes
    )
    if len(fixed) != 112:
        raise BuildError("internal: optional header fixed part is %d bytes" % len(fixed))
    out[opt:opt + 112] = fixed
    for index, (rva, size) in enumerate(dirs):
        struct.pack_into("<II", out, opt + 112 + index * 8, rva, size)

    # Section table.
    table_off = opt + 240
    for index, (name, rva, vsize, roff, rsize, chars, _data) in enumerate(table):
        entry = table_off + index * 40
        struct.pack_into("<8sIIIIIIHHI", out, entry,
                         name.encode("ascii").ljust(8, b"\x00"),
                         vsize, rva, rsize, roff,
                         0, 0, 0, 0, chars)
    if table_off + 40 * len(table) > HEADERS_SIZE:
        raise BuildError("section table does not fit in headers")

    # Section bodies.
    image = bytearray(file_size)
    image[:HEADERS_SIZE] = out
    for name, rva, vsize, roff, rsize, chars, data in table:
        image[roff:roff + len(data)] = data
    return bytes(image)


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

def parse_define(text):
    if "=" not in text:
        raise argparse.ArgumentTypeError("expected NAME=INT, got %r" % text)
    name, value = text.split("=", 1)
    if not re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", name):
        raise argparse.ArgumentTypeError("bad symbol name %r" % name)
    try:
        return name, int(value, 0)
    except ValueError:
        raise argparse.ArgumentTypeError("not an integer: %r" % value)


def parse_string(text):
    if "=" not in text:
        raise argparse.ArgumentTypeError("expected NAME=TEXT, got %r" % text)
    name, value = text.split("=", 1)
    if not re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", name):
        raise argparse.ArgumentTypeError("bad symbol name %r" % name)
    return name, value


def main(argv):
    parser = argparse.ArgumentParser(description="Build a RISC-V64 PE32+ console fixture.")
    parser.add_argument("source", help="guest assembly source (.S)")
    parser.add_argument("-o", "--output", required=True, help="output PE file")
    parser.add_argument("--define", action="append", default=[], type=parse_define,
                        metavar="NAME=INT", help="integer .set symbol visible to the source")
    parser.add_argument("--string", action="append", default=[], type=parse_string,
                        metavar="NAME=TEXT", help="NUL-terminated string label (C escapes allowed)")
    parser.add_argument("--workdir", help="intermediate directory (default: OUTPUT.work)")
    parser.add_argument("--elf-out", help="copy of the linked RISC-V ELF64 intermediate")
    parser.add_argument("--raw-words", action="store_true",
                        help="negative fixtures only: do not require every .text word to be a "
                             "32-bit encoding (allows raw compressed or illegal words)")
    parser.add_argument("--entry", default="_start", help="entry symbol (default _start)")
    args = parser.parse_args(argv)

    source_path = os.path.abspath(args.source)
    with open(source_path, "r") as handle:
        source_text = handle.read()
    imports = scan_imports(source_text)
    check_import_references(source_text, imports)
    layout = IdataLayout(imports)

    workdir = os.path.abspath(args.workdir or (args.output + ".work"))
    os.makedirs(workdir, exist_ok=True)

    write_defs(os.path.join(workdir, "defs.inc"), args.define)
    write_strings(os.path.join(workdir, "strings.inc"), args.string)
    write_idata(os.path.join(workdir, "idata.S"), layout)
    write_linker_script(os.path.join(workdir, "image.lds"))

    mc_flags = ["-triple=riscv64", "-mattr=-c,-relax", "-filetype=obj", "-I", workdir]
    guest_obj = os.path.join(workdir, "guest.o")
    idata_obj = os.path.join(workdir, "idata.o")
    run(["llvm-mc"] + mc_flags + [source_path, "-o", guest_obj])
    run(["llvm-mc"] + mc_flags + [os.path.join(workdir, "idata.S"), "-o", idata_obj])

    linked = os.path.join(workdir, "image.elf")
    run(["ld.lld", "-m", "elf64lriscv", "--no-relax", "-T", os.path.join(workdir, "image.lds"),
         "-o", linked, guest_obj, idata_obj])

    with open(linked, "rb") as handle:
        elf = Elf64(handle.read())
    if ".idata" not in elf.sections:
        raise BuildError("linked image has no .idata section")
    idata_bytes = elf.section_bytes(".idata")
    expected_idata = expected_idata_bytes(layout)
    if idata_bytes != expected_idata:
        raise BuildError(".idata from link (%d bytes) does not match layout (%d bytes)"
                         % (len(idata_bytes), len(expected_idata)))

    pe = build_pe(elf, args.entry, layout, idata_bytes, args.raw_words)
    out_dir = os.path.dirname(os.path.abspath(args.output))
    if out_dir and not os.path.isdir(out_dir):
        os.makedirs(out_dir)
    with open(args.output, "wb") as handle:
        handle.write(pe)
    if args.elf_out:
        with open(linked, "rb") as src, open(args.elf_out, "wb") as dst:
            dst.write(src.read())
    print("mkpe: wrote %s (%d bytes, %d imports)" % (args.output, len(pe), len(imports)))
    return 0


def expected_idata_bytes(layout):
    """Reference .idata image built directly from the layout (independent of
    the assembly text). The linked section must match it byte for byte."""
    blob = bytearray(layout.size)

    def put(offset, fmt, *values):
        struct.pack_into("<" + fmt, blob, offset, *values)

    for index, module in enumerate(layout.modules):
        put(20 * index, "IIIII",
            layout.rva(layout.ilt_off[module]), 0, 0,
            layout.rva(layout.dll_off[module]), layout.rva(layout.iat_off[module]))
    for module in layout.modules:
        for symbol in layout.symbols[module]:
            hn = layout.rva(layout.hn_off[(module, symbol)])
            put(layout.ilt_off[module] + 8 * layout.symbols[module].index(symbol), "Q", hn)
            put(layout.iat_off[module] + 8 * layout.symbols[module].index(symbol), "Q", hn)
        put(layout.ilt_off[module] + 8 * len(layout.symbols[module]), "Q", 0)
        put(layout.iat_off[module] + 8 * len(layout.symbols[module]), "Q", 0)
    for module in layout.modules:
        for symbol in layout.symbols[module]:
            off = layout.hn_off[(module, symbol)]
            name = symbol.encode("ascii") + b"\x00"
            blob[off + 2:off + 2 + len(name)] = name
        off = layout.dll_off[module]
        name = module.encode("ascii") + b"\x00"
        blob[off:off + len(name)] = name
    return bytes(blob)


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except BuildError as exc:
        sys.stderr.write("mkpe: error: %s\n" % exc)
        sys.exit(1)
