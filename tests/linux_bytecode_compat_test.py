"""Execute original and lowered compiler chunks in the independent Luau VM."""
import ctypes as C
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def decoded(data):
    """Remove Roblox's opcode encoding for the stock VM, retaining its format."""
    out = bytearray(data)
    at = 0
    header = (ROOT / "native/bytecode_layout.h").read_text()
    lengths = [int(x) for x in re.findall(r"\d+", header.split("opcode_lengths[] = {")[1])]
    opcodes = []

    def take(size):
        nonlocal at
        assert 0 <= size <= len(data) - at
        value = data[at:at + size]
        at += size
        return value

    def byte(): return take(1)[0]

    def var():
        value = shift = 0
        while True:
            b = byte()
            value |= (b & 127) << shift
            if b < 128: return value
            shift += 7
            assert shift < 70

    version, types = byte(), byte()
    for _ in range(var()): take(var())
    if types == 3:
        while byte(): var()
    for _ in range(var()):
        end = None
        if version >= 12:
            size = var()
            end = at + size
        take(4)
        flags = byte()
        take(var())
        sizecode = var()
        pc = 0
        while pc < sizecode:
            op = data[at] * 203 & 255
            out[at] = op
            opcodes.append(op)
            take(4 * lengths[op])
            pc += lengths[op]
        assert pc == sizecode
        for _ in range(var()):
            kind = byte()
            if kind == 0: pass
            elif kind == 1: byte()
            elif kind == 2: take(8)
            elif kind in (3, 6): var()
            elif kind == 4: take(4)
            elif kind == 5:
                for _ in range(var()): var()
            elif kind == 7: take(16)
            elif kind == 8:
                for _ in range(var()): var(); take(4)
            elif kind == 9: byte(); var()
            elif kind == 10:
                var()
                properties, methods = var(), var()
                for _ in range(properties + methods): var()
            elif kind == 11: take(32)
            else: raise AssertionError(kind)
        for _ in range(var()): var()
        var(); var()
        if byte():
            gap = byte()
            take(sizecode + (((sizecode - 1) >> gap) + 1) * 4)
        if byte():
            for _ in range(var()): var(); var(); var(); byte()
            for _ in range(var()): var()
        if version >= 11:
            for _ in range(var()): byte(); var()
        if version >= 12:
            if flags & 8: var()
            assert at == end
    var()
    assert at == len(data)
    return bytes(out), opcodes


class BytecodeCompatibilityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        build = ROOT / "luau/build"
        cache = build / "CMakeCache.txt"
        if not cache.is_file():
            raise unittest.SkipTest("configure the host Luau build before running VM equivalence tests")
        cmake = shutil.which("cmake")
        if not cmake:
            configured = re.search(r"^CMAKE_COMMAND:INTERNAL=(.+)$", cache.read_text(), re.M)
            if configured and Path(configured[1]).is_file(): cmake = configured[1]
        if not cmake:
            raise unittest.SkipTest("CMake is required for VM equivalence tests")
        cls.temporary = tempfile.TemporaryDirectory(prefix="basalt-bytecode-compat-")
        tmp = Path(cls.temporary.name)
        cls.library = tmp / "compat.so"
        subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-shared", "-fPIC", "-Inative",
                        "native/linux_bytecode_compat.c", "-o", str(cls.library)], cwd=ROOT, check=True)
        cls.lib = C.CDLL(str(cls.library))
        cls.lib.linux_bytecode_compat.argtypes = [C.c_char_p, C.c_size_t, C.POINTER(C.c_void_p),
                                                 C.POINTER(C.c_size_t), C.c_char_p, C.c_size_t]
        cls.libc = C.CDLL(None)
        cls.libc.free.argtypes = [C.c_void_p]
        subprocess.run([cmake, "--build", str(build), "--target", "Luau.VM", "Luau.Compiler", "-j", "4"],
                       check=True, capture_output=True)
        cls.tool = tmp / "compat-tool"
        command = ["c++", "-std=c++17", "-O2", "tests/linux_bytecode_compat_tool.cpp"]
        command += ["-Iluau/" + name + "/include" for name in ("Compiler", "Ast", "Bytecode", "Common", "VM")]
        command += [str(build / ("libLuau." + name + ".a")) for name in ("Compiler", "Ast", "VM", "Bytecode", "Common")]
        subprocess.run(command + ["-o", str(cls.tool)], cwd=ROOT, check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls): cls.temporary.cleanup()

    def compile(self, source, version=13):
        return subprocess.run([str(self.tool), "compile", str(version)], input=source.encode(), capture_output=True, check=True).stdout

    def convert(self, data):
        result, size, error = C.c_void_p(), C.c_size_t(), C.create_string_buffer(192)
        status = self.lib.linux_bytecode_compat(data, len(data), C.byref(result), C.byref(size), error, len(error))
        value = C.string_at(result, size.value) if result else None
        if result: self.libc.free(result)
        return status, value, error.value.decode()

    def run_chunk(self, data):
        plain, _ = decoded(data)
        return subprocess.run([str(self.tool), "run"], input=plain, capture_output=True, check=True).stdout

    def test_execution_and_debug_equivalence(self):
        source = '''local function multi(n, ...) return n * 2, nil, ..., nil end
local function caller(f, n)
    local sum = 0
    for i=1,n do
        local a, b, c, d = f(i, 'tail')
        assert(b == nil and c == 'tail' and d == nil)
        if i % 2 == 0 then sum += a else sum -= a end
    end
    return sum
end
local captured = 7
local function closure(n) captured += n; return captured end
local t = {one=1, two=2, three=3}
local sum=0
for _,v in t do sum += math.abs(v) end
local co=coroutine.create(function() coroutine.yield(closure(3)); return closure(5) end)
local ok, first=coroutine.resume(co)
local _, second=coroutine.resume(co)
return caller(multi, 31), select('#',multi(2,'tail')), sum, ok, first, second, debug.info(caller,'l'), nil
'''
        for version in (11, 12, 13):
            with self.subTest(version=version):
                original = self.compile(source, version)
                status, lowered, error = self.convert(original)
                self.assertEqual(status, 1, error)
                self.assertEqual(lowered[0], 9)
                self.assertLess(len(lowered), len(original))
                before = self.run_chunk(original)
                self.assertTrue(before.startswith(b'status=0 count=8\n'), before)
                self.assertEqual(self.run_chunk(lowered), before)
                self.assertEqual(self.convert(original)[1], lowered)

    def test_errors_and_exact_vectors(self):
        for source in ('local function f() error("intentional", 0) end; return f()',
                       'local v=vector.create(1.5,-2,8); return v.X,v.Y,v.Z',
                       'local f=setmetatable({}, {__call=function(_,v) return v+3 end}); return f(7)'):
            original = self.compile(source)
            status, lowered, error = self.convert(original)
            self.assertEqual(status, 1, error)
            self.assertEqual(self.run_chunk(lowered), self.run_chunk(original))

    def test_precision_fallback_and_truncated_input(self):
        original = self.compile('return vector.create(0.1,0,0)')
        # The compiler folds this literal to a float vector even in a v13
        # chunk. Re-encode that constant using v13's double-vector tag.
        marker = bytes([7]) + struct.pack('<4f', 0.1, 0, 0, 0)
        self.assertEqual(original.count(marker), 1)
        self.assertEqual(original[:5], bytes([13, 3, 0, 0, 1]))
        self.assertLess(original[5], 112)  # single-byte size of the one prototype
        for number in (1.5, 0.1):
            double = bytearray(original.replace(marker, bytes([11]) + struct.pack('<4d', number, 0, 0, 0)))
            double[5] += 16
            status, lowered, error = self.convert(bytes(double))
            if number == 1.5:
                self.assertEqual(status, 1, error)
                self.assertEqual(self.run_chunk(lowered), self.run_chunk(bytes(double)))
            else:
                self.assertEqual((status, lowered), (0, None))
        original = self.compile('local function f(a) return a + 1 end; return f(3)')
        for size in range(1, len(original)):
            status, value, error = self.convert(original[:size])
            self.assertEqual(status, -1, (size, status, error))
            self.assertIsNone(value)
        self.assertEqual(self.convert(original + bytes(24))[0], 1)
        self.assertEqual(self.convert(original + bytes(7))[:2], (0, None))
        self.assertEqual(self.convert(self.compile('return 5', 9))[:2], (0, None))


if __name__ == '__main__': unittest.main()
