#!/usr/bin/env python3
"""Encrypt compiled Luau before embedding it in the native library."""
import argparse
import secrets
import struct
from pathlib import Path


def rotate(value: int, count: int) -> int:
    return ((value << count) | (value >> (32 - count))) & 0xffffffff


def quarter_round(words: list[int], a: int, b: int, c: int, d: int) -> None:
    words[a] = (words[a] + words[b]) & 0xffffffff
    words[d] = rotate(words[d] ^ words[a], 16)
    words[c] = (words[c] + words[d]) & 0xffffffff
    words[b] = rotate(words[b] ^ words[c], 12)
    words[a] = (words[a] + words[b]) & 0xffffffff
    words[d] = rotate(words[d] ^ words[a], 8)
    words[c] = (words[c] + words[d]) & 0xffffffff
    words[b] = rotate(words[b] ^ words[c], 7)


def chacha20_block(key: bytes, nonce: bytes, counter: int) -> bytes:
    if len(key) != 32 or len(nonce) != 12 or not 0 <= counter < 2**32:
        raise ValueError("invalid ChaCha20 key, nonce, or counter")
    state = list(struct.unpack("<16I", b"expand 32-byte k" + key + struct.pack("<I", counter) + nonce))
    words = state.copy()
    for _ in range(10):
        for a, b, c, d in ((0, 4, 8, 12), (1, 5, 9, 13), (2, 6, 10, 14), (3, 7, 11, 15),
                           (0, 5, 10, 15), (1, 6, 11, 12), (2, 7, 8, 13), (3, 4, 9, 14)):
            quarter_round(words, a, b, c, d)
    return struct.pack("<16I", *((word + original) & 0xffffffff for word, original in zip(words, state)))


def crypt(data: bytes, key: bytes, nonce: bytes) -> bytes:
    result = bytearray(data)
    for offset in range(0, len(result), 64):
        block = chacha20_block(key, nonce, 1 + offset // 64)
        for i in range(min(64, len(result) - offset)):
            result[offset + i] ^= block[i]
    return bytes(result)


def scramble(data: bytes, ciphertext: bytes) -> bytes:
    """Mask key material with the payload so no constant in the binary unlocks it."""
    return bytes(byte ^ ciphertext[index] for index, byte in enumerate(data))


def c_array(name: str, data: bytes) -> list[str]:
    rows = [f"static const uint8_t {name}[] = {{"]
    for offset in range(0, len(data), 16):
        rows.append("    " + ", ".join(f"0x{byte:02x}" for byte in data[offset:offset + 16]) + ",")
    return rows + ["};"]


def main(name: str = "scheduler") -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", type=Path)
    parser.add_argument("header", type=Path)
    args = parser.parse_args()
    data = args.command.read_bytes()
    if not data.startswith(b"EXEC_BIN\n") or len(data) <= 9:
        parser.error("expected a non-empty EXEC_BIN command")

    key, nonce = secrets.token_bytes(32), secrets.token_bytes(12)
    lines = ["#pragma once", "#include <stdint.h>", ""]
    ciphertext = crypt(data[9:], key, nonce)
    lines += c_array(f"{name}_bytecode", ciphertext)
    lines += [""] + c_array(f"{name}_key", scramble(key, ciphertext))
    lines += [""] + c_array(f"{name}_nonce", scramble(nonce, ciphertext))
    args.header.write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
