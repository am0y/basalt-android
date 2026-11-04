#!/usr/bin/env python3
"""Rank possible function matches across two ARM64 Roblox engines.

This is a discovery aid, not a verified offset table. Exact instruction windows
can match another function or miss functions changed by the compiler. Never
install a hook from its output without checking control flow and data layouts.
"""
import argparse
import json
import re
import struct
from collections import defaultdict
from pathlib import Path


def code_range(data):
    if data[:4] != b"\x7fELF" or struct.unpack_from("<H", data, 18)[0] != 183:
        raise ValueError("expected a 64-bit little-endian ARM64 ELF")
    offset = struct.unpack_from("<Q", data, 32)[0]
    # ELF64 program header size/count live at offsets 54/56.
    entry_size, count = struct.unpack_from("<HH", data, 54)
    for n in range(count):
        p = offset + n * entry_size
        kind, flags = struct.unpack_from("<II", data, p)
        if kind == 1 and flags & 1:
            file_start, virtual, _, file_size = struct.unpack_from("<QQQQ", data, p + 8)
            return file_start, virtual, file_size
    raise ValueError("no executable segment")


def offsets_from_source(path):
    source = path.read_text()
    arm = source.split("#else /* __aarch64__ */", 1)[-1]
    return sorted({int(match, 16) for match in re.findall(r"0x([0-9a-fA-F]{7,8})", arm)
                  if int(match, 16) < 0x6400000})


def candidate_offsets(old, new, old_address, old_text, new_text):
    old_file, old_rva, old_size = old_text
    new_file, new_rva, new_size = new_text
    pos = old_file + old_address - old_rva
    if pos < old_file or pos + 256 > old_file + old_size:
        return []
    text = new[new_file:new_file + new_size]
    votes = defaultdict(set)
    # Search a few distinct anchors, then validate the rest at each candidate.
    # A full-image find for every 4-byte alignment is prohibitively expensive.
    for relative in (4, 20, 40, 60, 84, 108, 136, 160):
        needle = old[pos + relative:pos + relative + 20]
        if len(needle) != 20 or needle == b"\0" * 20:
            continue
        start = 0
        hits = 0
        while (hit := text.find(needle, start)) != -1 and hits < 32:
            start = hit + 1
            hits += 1
            if hit % 4 == 0:
                proposed = new_rva + hit - relative
                if proposed >= new_rva:
                    votes[proposed].add(relative)
    for proposed, windows in votes.items():
        pos_new = new_file + proposed - new_rva
        if pos_new < new_file or pos_new + 192 + 20 > new_file + new_size:
            windows.clear()
            continue
        for relative in range(0, 192, 4):
            if old[pos + relative:pos + relative + 20] == new[pos_new + relative:pos_new + relative + 20]:
                windows.add(relative)
    return [{"rva": hex(rva), "windows": len(windows),
             "relative_windows": sorted(windows)}
            for rva, windows in sorted(votes.items(), key=lambda item: -len(item[1]))[:5]
            if len(windows) >= 5]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("old", type=Path)
    parser.add_argument("new", type=Path)
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[1] / "native/engine.c")
    parser.add_argument("--offset", type=lambda value: int(value, 0), action="append",
                        help="specific old RVA; repeat to inspect function entry points")
    args = parser.parse_args()
    old, new = args.old.read_bytes(), args.new.read_bytes()
    old_text, new_text = code_range(old), code_range(new)
    offsets = args.offset if args.offset else offsets_from_source(args.source)
    print(json.dumps({hex(rva): candidate_offsets(old, new, rva, old_text, new_text)
                      for rva in offsets}, indent=2))


if __name__ == "__main__":
    main()
