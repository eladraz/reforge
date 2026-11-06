#!/usr/bin/env python3
"""Helpers of scripts/e2e.sh that need to look inside an ELF64 file (stdlib only).

  e2e_check.py blocks <elf> <target-dump.txt>
      every instruction line of --dump-target (text offset + bytes) must be
      the very bytes found in the executable at reforge_text + offset
  e2e_check.py msgloc <elf> <message>
      every occurrence of the message bytes in the executable lies in the
      reforge_meta blob (the guest image data) and none in reforge_text
"""
import re
import struct
import sys


class Elf:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        d = self.data
        if d[:4] != b"\x7fELF" or d[4] != 2:
            raise SystemExit("not an ELF64 file: " + path)
        (self.phoff, self.shoff) = struct.unpack_from("<QQ", d, 0x20)
        (self.phentsize, self.phnum, self.shentsize, self.shnum, self.shstrndx) = \
            struct.unpack_from("<HHHHH", d, 0x36)
        self.loads = []
        for i in range(self.phnum):
            off = self.phoff + i * self.phentsize
            (p_type, p_flags, p_offset, p_vaddr, _pa, p_filesz, _ms, _al) = \
                struct.unpack_from("<IIQQQQQQ", d, off)
            if p_type == 1:
                self.loads.append((p_vaddr, p_offset, p_filesz))
        self.sections = []
        for i in range(self.shnum):
            off = self.shoff + i * self.shentsize
            (name, typ, flags, addr, offset, size, link, info, align, entsize) = \
                struct.unpack_from("<IIQQQQIIQQ", d, off)
            self.sections.append((name, typ, flags, addr, offset, size, link, info, entsize))
        self.symbols = {}
        for (name, typ, flags, addr, offset, size, link, info, entsize) in self.sections:
            if typ != 2:  # SHT_SYMTAB
                continue
            strtab = self.sections[link]
            for k in range(size // 24):
                (st_name, _info, _other, _shndx, st_value, st_size) = \
                    struct.unpack_from("<IBBHQQ", d, offset + 24 * k)
                end = d.index(b"\0", strtab[4] + st_name)
                self.symbols[d[strtab[4] + st_name:end].decode()] = (st_value, st_size)

    def file_offset(self, vaddr):
        for (va, off, size) in self.loads:
            if va <= vaddr < va + size:
                return off + (vaddr - va)
        raise SystemExit("address 0x%x is not in a loaded segment" % vaddr)

    def symbol_range(self, name):
        if name not in self.symbols:
            raise SystemExit("symbol %s not found" % name)
        (addr, size) = self.symbols[name]
        start = self.file_offset(addr)
        return (addr, size, start, start + size)


def blocks(elf_path, dump_path):
    elf = Elf(elf_path)
    (_a, tsize, tstart, _te) = elf.symbol_range("reforge_text")
    count = 0
    pattern = re.compile(r"^  ([0-9a-f]{8}): ((?:[0-9a-f]{2} )+)")
    for line in open(dump_path):
        m = pattern.match(line)
        if not m:
            continue
        offset = int(m.group(1), 16)
        raw = bytes(int(b, 16) for b in m.group(2).split())
        if offset + len(raw) > tsize:
            raise SystemExit("dump line outside reforge_text: " + line.rstrip())
        actual = elf.data[tstart + offset:tstart + offset + len(raw)]
        if actual != raw:
            raise SystemExit("bytes differ at text+0x%x: dump %s, elf %s" %
                             (offset, raw.hex(), actual.hex()))
        count += 1
    if count == 0:
        raise SystemExit("no instruction lines in " + dump_path)
    print("%d generated instructions of the dump are byte-identical to reforge_text of the executable" % count)


def msgloc(elf_path, message):
    elf = Elf(elf_path)
    needle = message.encode().decode("unicode_escape").encode("latin-1")
    (_a, _s, mstart, mend) = elf.symbol_range("reforge_meta")
    (_a, _s, tstart, tend) = elf.symbol_range("reforge_text")
    hits = []
    pos = elf.data.find(needle)
    while pos >= 0:
        hits.append(pos)
        pos = elf.data.find(needle, pos + 1)
    if not hits:
        raise SystemExit("message not found in the executable (guest data must be embedded)")
    for h in hits:
        if tstart <= h < tend:
            raise SystemExit("message found inside the translated code at file offset 0x%x" % h)
        if not (mstart <= h < mend):
            raise SystemExit("message found outside reforge_meta at file offset 0x%x" % h)
    print("%d occurrence(s) of %r, all inside the reforge_meta guest image, none in reforge_text" %
          (len(hits), needle))


def main(argv):
    if len(argv) >= 4 and argv[1] == "blocks":
        blocks(argv[2], argv[3])
    elif len(argv) >= 4 and argv[1] == "msgloc":
        msgloc(argv[2], argv[3])
    else:
        raise SystemExit(__doc__)


main(sys.argv)
