"""Generate C instruction metadata from the compiler headers we build against."""
import re
from pathlib import Path

common = Path(__file__).resolve().parent.parent / "luau/Common/include/Luau"
source = (common / "Bytecode.h").read_text()
enum = source.split("enum LuauOpcode", 1)[1].split("};", 1)[0]
names = re.findall(r"^\s*(LOP_[A-Z0-9_]+)\s*,", enum, re.M)
lengths = (common / "BytecodeUtils.h").read_text().split("inline int getOpLength", 1)[1].split("default:", 1)[0]
aux = set(re.findall(r"case (LOP_\w+):", lengths))
header = ["#pragma once", "// Generated from Luau headers.", "enum {"]
header += [f"    {name} = {i}," for i, name in enumerate(names)]
header += [f"    OPCODE_COUNT = {len(names)}", "};", "static const unsigned char opcode_lengths[] = {"]
header += ["    " + ", ".join(str(2 if name in aux else 1) for name in names) + ",", "};\n"]
Path(__file__).with_name("bytecode_layout.h").write_text("\n".join(header))
