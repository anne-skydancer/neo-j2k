#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, Anne Skydancer (neo-j2k)
"""Check the static library's objects for global state, and the CPU-check object for newer instructions.

- No writable global data: no .data or .bss with contents, no thread-local storage.
- No static initialisers or finalisers: no .CRT$XC*/.CRT$XI* (MSVC) or .init_array/.ctors/
  .fini_array/.dtors (ELF).
  So nothing of the library runs before main() or before a host's CPU check, and nothing is shared
  between decoders behind the caller's back (plan section 2.5; api.md section 2.2).
- No link-time code generation: no MSVC /GL (LTCG) object, no LLVM bitcode, no GCC LTO sections
  (.gnu.lto_*). Microsoft advises against shipping /GL objects, and GCC LTO objects only link with
  the GCC that wrote them (plan section 2.5).
- The object of nj2k_cpu_supported() holds no instruction beyond baseline x86-64 (no VEX or EVEX
  encoding, no BMI, LZCNT, POPCNT, MOVBE, SSE3, SSSE3, SSE4, LAHF/SAHF in 64-bit mode or
  CMPXCHG16B): it must run on any x86-64 processor. With --baseline-library, every object of the
  library is held to the same (the scalar build, NJ2K_SIMD=SCALAR).

usage: check_objects.py --library neoj2k_static.lib|libneoj2k.a [--cpu-check-objects OBJ...]
                        [--baseline-library]
       check_objects.py --selftest

Archives and objects are read by this script (ar, COFF, ELF64); only the disassembly uses a tool:
dumpbin (Windows) or objdump (elsewhere).
"""
import argparse
import glob
import os
import re
import shutil
import struct
import subprocess
import sys

COFF_FORBIDDEN = (".data", ".bss", ".tls", ".CRT$XC", ".CRT$XI", ".CRT$XP", ".CRT$XT")
ELF_FORBIDDEN = (".data", ".bss", ".tdata", ".tbss", ".init_array", ".fini_array", ".ctors", ".dtors",
                 ".preinit_array")
ELF_ALLOWED = (".data.rel.ro",)  # read-only after relocation
ELF_LTO = (".gnu.lto_",)  # GCC's LTO bytecode
LLVM_BITCODE = (b"BC\xc0\xde", b"\xde\xc0\x17\x0b")  # raw and wrapped LLVM bitcode (Clang -flto, clang-cl /GL)


def ar_members(data):
    if data[:8] != b"!<arch>\n":
        return [("(object)", data)]
    members, pos, longnames = [], 8, b""
    while pos + 60 <= len(data):
        hdr = data[pos:pos + 60]
        name = hdr[:16].decode("latin-1").strip()
        size = int(hdr[48:58].decode("ascii").strip())
        body = data[pos + 60:pos + 60 + size]
        pos += 60 + size + (size & 1)
        if name == "//":
            longnames = body
            continue
        if name in ("/", "/SYM64/", "__.SYMDEF"):
            continue
        if name.startswith("/") and name[1:].isdigit():
            # long names: NUL-terminated (MSVC lib) or "/\n"-terminated (GNU ar)
            start = int(name[1:])
            ends = [e for e in (longnames.find(b"\0", start), longnames.find(b"\n", start)) if e >= 0]
            name = longnames[start:min(ends) if ends else None].decode("latin-1").rstrip("/")
        members.append((name.rstrip("/"), body))
    return members


def coff_sections(obj):
    """[(index, name, size)] and {section index: [symbol names]}, or None for a non-COFF object."""
    machine, nsec = struct.unpack_from("<HH", obj, 0)
    if machine == 0 and nsec == 0xFFFF:
        return None  # an import or LTCG object, not plain COFF
    symptr, nsym = struct.unpack_from("<II", obj, 8)
    opt = struct.unpack_from("<H", obj, 16)[0]
    strtab = symptr + 18 * nsym

    def cstr(o):
        return obj[o:obj.index(b"\0", o)].decode("latin-1")

    sections = []
    for i in range(nsec):
        s = 20 + opt + 40 * i
        name = obj[s:s + 8].rstrip(b"\0").decode("latin-1")
        if name.startswith("/"):
            name = cstr(strtab + int(name[1:]))
        size = struct.unpack_from("<I", obj, s + 16)[0]
        flags = struct.unpack_from("<I", obj, s + 36)[0]
        if flags & 0x00000800:  # IMAGE_SCN_LNK_REMOVE: .drectve and the like
            continue
        sections.append((i + 1, name, size))
    symbols = {}
    i = 0
    while i < nsym:
        e = symptr + 18 * i
        short = obj[e:e + 8]
        sym = cstr(strtab + struct.unpack_from("<I", short, 4)[0]) if short[:4] == b"\0\0\0\0" else \
            short.rstrip(b"\0").decode("latin-1")
        secno, _, sclass, naux = struct.unpack_from("<hHBB", obj, e + 12)
        if secno > 0 and sclass in (2, 3) and not sym.startswith("."):  # external or static, not a section symbol
            symbols.setdefault(secno, []).append(sym)
        i += 1 + naux
    return sections, symbols


def elf_sections(obj):
    """[(index, name, size)] and {section index: [symbol names]}."""
    if obj[4] != 2 or obj[5] != 1:
        raise ValueError("only little-endian ELF64 objects are supported")
    shoff = struct.unpack_from("<Q", obj, 0x28)[0]
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", obj, 0x3A)

    def sh(i):
        return struct.unpack_from("<IIQQQQIIQQ", obj, shoff + i * shentsize)

    def cstr(o):
        return obj[o:obj.index(b"\0", o)].decode("latin-1")

    stroff = sh(shstrndx)[4]
    sections, symbols = [], {}
    for i in range(shnum):
        name_off, typ, flags, addr, off, size, link = sh(i)[:7]
        sections.append((i, cstr(stroff + name_off), size))
        if typ == 2:  # SHT_SYMTAB
            names_at = sh(link)[4]
            for k in range(size // 24):
                st_name, st_info, st_other, st_shndx = struct.unpack_from("<IBBH", obj, off + 24 * k)
                if st_name and (st_info & 0xF) in (1, 6) and 0 < st_shndx < 0xFF00:  # objects, TLS
                    symbols.setdefault(st_shndx, []).append(cstr(names_at + st_name))
    return sections, symbols


def problems_in(name, obj):
    found = []
    if obj[:4] in LLVM_BITCODE:
        return ["%s: LLVM bitcode (link-time code generation): the static library must not use LTO" % name]
    if obj[:4] == b"\x7fELF":
        sections, symbols = elf_sections(obj)
        lto = sorted(set(sec for _, sec, _ in sections if sec.startswith(ELF_LTO)))
        if lto:
            found.append("%s: GCC LTO sections (%s): the static library must not use LTO" % (name, ", ".join(lto[:3])))
        forbidden = lambda s: s.startswith(ELF_FORBIDDEN) and not s.startswith(ELF_ALLOWED)
    else:
        parsed = coff_sections(obj)
        if parsed is None:
            return ["%s: not a plain COFF object (LTCG /GL?): the static library must not use /GL" % name]
        sections, symbols = parsed
        forbidden = lambda s: s.startswith(COFF_FORBIDDEN)
    for index, sec, size in sections:
        if size and forbidden(sec):
            syms = symbols.get(index, [])
            found.append("%s: section %s holds %d bytes%s" % (
                name, sec, size, " (" + ", ".join(syms) + ")" if syms else ""))
    return found


# ---- the CPU-check object ------------------------------------------------------------------------

NOT_VEX_V = {"verr", "verw", "vmcall", "vmclear", "vmfunc", "vmlaunch", "vmptrld", "vmptrst", "vmread",
             "vmresume", "vmwrite", "vmxoff", "vmxon"}
NEWER = {
    # BMI1, BMI2, LZCNT, MOVBE, POPCNT
    "andn", "bextr", "blsi", "blsmsk", "blsr", "bzhi", "mulx", "pdep", "pext", "rorx", "sarx", "shlx", "shrx",
    "lzcnt", "tzcnt", "movbe", "popcnt",
    # SSE3
    "movddup", "movshdup", "movsldup", "lddqu", "haddps", "haddpd", "hsubps", "hsubpd", "addsubps", "addsubpd",
    "fisttp", "monitor", "mwait",
    # SSSE3
    "pshufb", "phaddw", "phaddd", "phaddsw", "phsubw", "phsubd", "phsubsw", "pmaddubsw", "pmulhrsw", "palignr",
    "pabsb", "pabsw", "pabsd", "psignb", "psignw", "psignd",
    # SSE4.1
    "pblendw", "blendps", "blendpd", "blendvps", "blendvpd", "pblendvb", "dpps", "dppd", "mpsadbw", "phminposuw",
    "extractps", "insertps", "pextrb", "pextrd", "pextrq", "pinsrb", "pinsrd", "pinsrq", "pmaxsb", "pmaxsd",
    "pmaxud", "pmaxuw", "pminsb", "pminsd", "pminud", "pminuw", "pmovsxbw", "pmovsxbd", "pmovsxbq", "pmovsxwd",
    "pmovsxwq", "pmovsxdq", "pmovzxbw", "pmovzxbd", "pmovzxbq", "pmovzxwd", "pmovzxwq", "pmovzxdq", "pmuldq",
    "pmulld", "ptest", "roundps", "roundpd", "roundss", "roundsd", "packusdw", "pcmpeqq", "movntdqa",
    # SSE4.2, CLMUL, AES, SHA, ADX, RDRAND, RDSEED
    "pcmpgtq", "crc32", "pcmpestri", "pcmpestrm", "pcmpistri", "pcmpistrm", "pclmulqdq", "aesenc", "aesenclast",
    "aesdec", "aesdeclast", "aesimc", "aeskeygenassist", "sha1rnds4", "sha1nexte", "sha1msg1", "sha1msg2",
    "sha256rnds2", "sha256msg1", "sha256msg2", "adcx", "adox", "rdrand", "rdseed",
    # missing from the first x86-64 processors: LAHF/SAHF in 64-bit mode, CMPXCHG16B
    "lahf", "sahf", "cmpxchg16b",
}
DUMPBIN_INSN = re.compile(r"^\s*[0-9A-Fa-f]+:\s+((?:[0-9A-Fa-f]{2}\s)+)\s*(\S+)")
OBJDUMP_INSN = re.compile(r"^\s*[0-9a-f]+:\s+(?:[0-9a-f]{2}\s)+\s*(\S+)")


def newer_instructions(lines, pattern, group):
    found, count = [], 0
    for line in lines:
        m = pattern.match(line)
        if not m:
            continue
        count += 1
        mnem = m.group(group).lower()
        if mnem in NEWER or (mnem.startswith("v") and mnem not in NOT_VEX_V):
            found.append(line.strip())
    return found, count


def find_dumpbin():
    exe = shutil.which("dumpbin")
    if exe:
        return exe
    vswhere = os.path.expandvars(r"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe")
    if os.path.exists(vswhere):
        root = subprocess.run([vswhere, "-latest", "-products", "*", "-property", "installationPath"],
                              capture_output=True, text=True).stdout.strip()
        hits = sorted(glob.glob(os.path.join(root, r"VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe")))
        if hits:
            return hits[-1]
    return None


def is_elf(path):
    """An ELF object, or an archive whose first object is one (GNU ar); otherwise COFF."""
    with open(path, "rb") as f:
        data = f.read(1 << 20)
    if data[:4] == b"\x7fELF":
        return True
    if data[:8] == b"!<arch>\n":
        members = ar_members(data)
        return bool(members) and members[0][1][:4] == b"\x7fELF"
    return False


def check_cpu_object(path):
    """An object, or a whole archive (dumpbin and objdump disassemble every member)."""
    if is_elf(path):
        tool = shutil.which("objdump")
        if not tool:
            return ["objdump not found: cannot disassemble " + path]
        lines = subprocess.run([tool, "-d", path], capture_output=True, text=True, check=True).stdout.splitlines()
        found, count = newer_instructions(lines, OBJDUMP_INSN, 1)
    else:
        tool = find_dumpbin()
        if not tool:
            return ["dumpbin not found: cannot disassemble " + path]
        lines = subprocess.run([tool, "/nologo", "/disasm", path], capture_output=True, text=True,
                               check=True).stdout.splitlines()
        found, count = newer_instructions(lines, DUMPBIN_INSN, 2)
    if count == 0:
        return ["%s: no instructions disassembled; did the tool's output format change?" % path]
    return ["%s: needs a newer processor: %s" % (path, line) for line in found]


def tiny_elf(section_names):
    """A minimal little-endian ELF64 relocatable object with empty sections of these names (selftest)."""
    all_names = [".shstrtab"] + list(section_names)
    strtab, offsets = b"\0", []
    for n in all_names:
        offsets.append(len(strtab))
        strtab += n.encode("ascii") + b"\0"
    shoff = 64 + len(strtab)
    shoff += (-shoff) % 8
    header = bytearray(64)
    header[:7] = b"\x7fELF\x02\x01\x01"
    struct.pack_into("<HHIQQQIHHHHHH", header, 16, 1, 62, 1, 0, 0, shoff, 0, 64, 0, 0, 64, len(all_names) + 1, 1)
    out = bytes(header) + strtab + bytes(shoff - 64 - len(strtab)) + bytes(64)
    out += struct.pack("<IIQQQQIIQQ", offsets[0], 3, 0, 0, 64, len(strtab), 0, 0, 1, 0)
    for off in offsets[1:]:
        out += struct.pack("<IIQQQQIIQQ", off, 1, 0, 0, 0, 0, 0, 0, 1, 0)
    return out


def selftest():
    ok = ["  0000000000000000: 48 83 EC 28        sub         rsp,28h",
          "  0000000000000007: 0F A2              cpuid",
          "  0000000000000009: 0F 01 D0           xgetbv"]
    bad = ["  0000000000000000: C5 F9 6F 05 00 00 00 00 vmovdqa      xmm0,xmmword ptr [x]",
           "  0000000000000012: F3 0F BD C0        lzcnt       eax,eax"]
    assert newer_instructions(ok, DUMPBIN_INSN, 2) == ([], 3)
    assert len(newer_instructions(bad, DUMPBIN_INSN, 2)[0]) == 2
    obj_ok = ["   0:\t0f a2                \tcpuid", "   2:\t0f 01 d0             \txgetbv"]
    obj_bad = ["   0:\tc5 fd 6f 07          \tvmovdqa (%rdi),%ymm0",
               "   4:\tf2 0f 12 c1          \tmovddup %xmm1,%xmm0",
               "   8:\t9f                   \tlahf"]
    assert newer_instructions(obj_ok, OBJDUMP_INSN, 1) == ([], 2)
    assert len(newer_instructions(obj_bad, OBJDUMP_INSN, 1)[0]) == 3
    # an ELF object with a GCC LTO section, and LLVM bitcode, are refused; a plain one passes
    assert problems_in("plain.o", tiny_elf([".text", ".rodata"])) == []
    assert problems_in("lto.o", tiny_elf([".text", ".gnu.lto_.symtab.0"]))
    assert problems_in("x.o", b"BC\xc0\xde" + bytes(60))
    assert problems_in("x.obj", b"\xde\xc0\x17\x0b" + bytes(60))
    print("check_objects: selftest passed")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--library")
    ap.add_argument("--cpu-check-objects", nargs="*", default=[])
    ap.add_argument("--baseline-library", action="store_true",
                    help="hold every object of the library to baseline x86-64 (the scalar build)")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    problems = []
    with open(a.library, "rb") as f:
        data = f.read()
    members = ar_members(data)
    for name, obj in members:
        problems += problems_in(name, obj)
    for obj in a.cpu_check_objects:
        problems += check_cpu_object(obj)
    if a.baseline_library:
        problems += check_cpu_object(a.library)
    for p in problems:
        print("check_objects: " + p, file=sys.stderr)
    if problems:
        return 1
    print("check_objects: %d objects without global state, initialisers or LTO; baseline x86-64: the CPU check "
          "(%d objects)%s" % (len(members), len(a.cpu_check_objects),
                              " and every object of the library" if a.baseline_library else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
