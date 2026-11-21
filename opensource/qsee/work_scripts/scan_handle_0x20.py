#!/usr/bin/env python3
"""Flag functions that both call QSEECom_* APIs and touch [xN, #32].

Heuristic evidence scan for OPEN-4 (does any vendor consumer read the
handle's +0x20 field, vendor 'ion_handle' / rebuilt 'heap_id'?):
reads llvm-objdump text on stdin (argv[1] may be /dev/stdin), prints
function name plus the matching instruction lines with small context.
x29/x30/x31(sp) are excluded as frame-pointer noise.
"""
import re
import sys

path = sys.argv[1]
lib = sys.argv[2]

FUNC = re.compile(r"^[0-9a-f]+ <(.+)>:\s*$")
CALL = re.compile(r"bl\s+[0-9a-f]+ <QSEECom")
FIELD = re.compile(r"\b(ldr|str)\s+w[0-9]+, \[(x(0|1|2|3|4|5|6|7|8|9|1[0-9]|2[0-8])), #(32|0x20)\]")

with open(path, "r", errors="replace") as f:
    lines = f.read().splitlines()

cur_name = None
cur = []
hits = 0

def flush(name, body):
    global hits
    has_call = any(CALL.search(l) for l in body)
    if not has_call:
        return
    idxs = [i for i, l in enumerate(body) if FIELD.search(l)]
    if not idxs:
        return
    hits += 1
    print(f"### {lib} :: {name}")
    for i in idxs:
        lo, hi = max(0, i - 3), min(len(body), i + 3)
        for j in range(lo, hi):
            mark = ">>" if j == i else "  "
            print(f"{mark} {body[j].rstrip()}")
        print("  --")
    print()

for line in lines:
    m = FUNC.match(line)
    if m:
        if cur_name:
            flush(cur_name, cur)
        cur_name, cur = m.group(1), []
    else:
        cur.append(line)
if cur_name:
    flush(cur_name, cur)

print(f"[{lib}] flagged functions: {hits}")
