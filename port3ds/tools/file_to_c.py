#!/usr/bin/env python3
"""file_to_c.py IN NAME OUT.c OUT.h: a binary file as a sized const char array (sizeof works on the extern)."""
import sys

src, name, out_c, out_h = sys.argv[1:5]
data = open(src, "rb").read()
body = ",\n".join("    " + ", ".join(f"(char)0x{b:02x}" for b in data[i:i + 16]) for i in range(0, len(data), 16))
header = out_h.split("/")[-1]
open(out_c, "w").write(f'#include "{header}"\n'
                       f"const char {name}[{len(data)}] __attribute__((aligned(8))) = {{\n{body}\n}};\n")
open(out_h, "w").write("#pragma once\n#ifdef __cplusplus\nextern \"C\"\n#endif\n"
                       f"const char {name}[{len(data)}];\n")
