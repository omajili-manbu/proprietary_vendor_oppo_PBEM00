#!/usr/bin/env python3
"""Service-table reconstruction for a given qseecomd binary (ELF64).

Generalized from reloc_table.py: instead of assuming the table starts at
.data start with 10 rows, scan .data for candidate rows whose +0x00 and
+0x10 slots are R_AARCH64_RELATIVE relocations pointing into .rodata, and
print each hit as a service row.  Pure struct parsing of untrusted data.
"""
import struct
import sys

PATH = sys.argv[1]

with open(PATH, "rb") as f:
    data = f.read()

e_phoff, = struct.unpack_from("<Q", data, 0x20)
e_shoff, = struct.unpack_from("<Q", data, 0x28)
e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 0x3A)

def cstr(buf, off):
    try:
        end = buf.index(b"\x00", off)
        return buf[off:end].decode("utf-8", "replace")
    except ValueError:
        return ""

secs = {}
shstr_off = e_shoff + e_shstrndx * e_shentsize
strtab_off, strtab_size = struct.unpack_from("<QQ", data, shstr_off + 0x18)
for i in range(e_shnum):
    off = e_shoff + i * e_shentsize
    name, typ, flags, addr, offset, size = struct.unpack_from("<IIQQQQ", data, off)
    secs[cstr(data[strtab_off:strtab_off + strtab_size], name)] = dict(
        addr=addr, offset=offset, size=size, typ=typ)

d = secs[".data"]
raw = data[d["offset"]:d["offset"] + d["size"]]
DATA_ADDR = d["addr"]

r = secs[".rela.dyn"]
relocs = {}
for i in range(r["size"] // 24):
    off, info, add = struct.unpack_from("<QQq", data, r["offset"] + i * 24)
    if (info & 0xFFFFFFFF) == 1027:  # R_AARCH64_RELATIVE
        relocs[off] = add

def rostr(vaddr):
    for nm in (".rodata", ".dynstr"):
        h = secs.get(nm)
        if h and h["addr"] <= vaddr < h["addr"] + h["size"]:
            o = h["offset"] + (vaddr - h["addr"])
            return cstr(data, o)
    return ""

ROW = 0x38
print(f"== {PATH}")
print(f"   .data addr=0x{DATA_ADDR:x} size=0x{d['size']:x}, "
      f"rela.dyn entries={r['size']//24}")

# Walk .data in 8-byte steps; a row starts where imm32@+8 (listener id)
# is followed 0x38 bytes later by another string-reloc row start.
rows = []
i = 0
data_words = d["size"] // 8
while i <= data_words - (ROW // 8):
    va = DATA_ADDR + i * 8
    name_ptr = relocs.get(va)
    so_ptr = relocs.get(va + 0x10)
    if name_ptr and so_ptr:
        nid = struct.unpack_from("<I", raw, va - DATA_ADDR + 8)[0]
        nid2 = struct.unpack_from("<I", raw, va - DATA_ADDR + 12)[0]
        sname = rostr(name_ptr)
        sso = rostr(so_ptr)
        if sname and sso and 0 < nid <= 0x10000:
            rows.append((va, sname, sso, nid, nid2))
            i += ROW // 8
            continue
    i += 1

for (va, sname, sso, nid, nid2) in rows:
    print(f"  @0x{va:x}  id=0x{nid:x}({nid}) flags=0x{nid2:x}  "
          f"so='{sso}'  name='{sname}'")
print(f"  total rows: {len(rows)}")
