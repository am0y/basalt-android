#!/usr/bin/env python3
"""Rank changed ARM64 function bodies by disassembled instruction sequence.

Use only for triage: compiler changes, common prologues and shared suffixes
produce false matches. Verify exact entrypoints, ABI, callers and behavior.
"""
import argparse
import json
from pathlib import Path

from capstone import CS_ARCH_ARM64, CS_MODE_ARM, Cs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("old", type=Path)
    parser.add_argument("new", type=Path)
    parser.add_argument("rva", type=lambda value: int(value, 0))
    parser.add_argument("--near", type=lambda value: int(value, 0), required=True)
    parser.add_argument("--radius", type=lambda value: int(value, 0), default=0x10000)
    args = parser.parse_args()
    old, new = args.old.read_bytes(), args.new.read_bytes()
    disassembler = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    pattern = [instruction.mnemonic for instruction in disassembler.disasm(
        old[args.rva:args.rva + 128], args.rva)][:24]
    lo = max(args.near - args.radius, 0)
    hi = min(args.near + args.radius + 128, len(new))
    candidate = [instruction.mnemonic for instruction in disassembler.disasm(new[lo:hi], lo)]
    scores = []
    for index in range(max(0, len(candidate) - len(pattern))):
        # Compare a whole function head rather than accepting only a prologue.
        same = sum(a == b for a, b in zip(pattern, candidate[index:index + len(pattern)]))
        if same < 8:
            continue
        consecutive = 0
        for a, b in zip(pattern, candidate[index:index + len(pattern)]):
            if a != b:
                break
            consecutive += 1
        scores.append((same, consecutive, lo + index * 4))
    scores.sort(reverse=True)
    print(json.dumps({"old": hex(args.rva), "pattern": pattern,
                      "candidates": [{"rva": hex(rva), "same_mnemonics": score,
                                      "prefix": prefix}
                                     for score, prefix, rva in scores[:10]]}, indent=2))


if __name__ == "__main__":
    main()
