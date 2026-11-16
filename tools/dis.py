#!/usr/bin/env python3
"""dis.py <game dir> <addr hex> [bytes]: disassemble at a guest address (with the analysis' rebasing)."""
import sys; sys.path.insert(0, __import__('os').path.dirname(__file__))
from analyze import Analysis
import capstone
an = Analysis(sys.argv[1])
a = int(sys.argv[2], 16); n = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x60
im = an.image_of(a)
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
for i in md.disasm(bytes(im.img[a - im.base:a - im.base + n]), a):
    print(f"{i.address:08X}  {i.mnemonic} {i.op_str}")
