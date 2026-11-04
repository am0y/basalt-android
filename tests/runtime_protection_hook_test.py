"""Validate ARM64 trampoline execution and startup rollback on host mappings."""
import ctypes
import mmap
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_SP, UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X30

ROOT = Path(__file__).resolve().parents[1]


class Hook(ctypes.Structure):
    _fields_ = [("target", ctypes.c_uint64), ("orig_bytes", ctypes.c_ubyte * 16),
                ("trampoline", ctypes.c_uint64), ("active", ctypes.c_int),
                ("detour", ctypes.c_uint64), ("allocation_size", ctypes.c_size_t)]


class Hooks(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="basalt-hook-test-")
        stage = Path(cls.tmp.name)
        (stage / "android").mkdir()
        (stage / "android/log.h").write_text("#pragma once\n")
        (stage / "fault.c").write_text('''
#include <errno.h>
#include <stddef.h>
int __real_mprotect(void*,size_t,int);
static unsigned current, failure;
void fail_at(unsigned n){ current=0; failure=n; }
int __wrap_mprotect(void* p,size_t n,int mode) {
    if (++current==failure) { errno=EACCES; return -1; }
    return __real_mprotect(p,n,mode);
}
''')
        subprocess.run(["cc", "-std=c11", "-D_GNU_SOURCE", "-DBASALT_DEV_BRIDGE=0",
                        "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror", "-I", str(stage),
                        str(ROOT / "native/hook.c"), str(stage / "fault.c"),
                        "-Wl,--wrap=mprotect", "-o", str(stage / "hook.so")], check=True)
        cls.lib = ctypes.CDLL(str(stage / "hook.so"))
        cls.lib.hook_arm64_prepare.argtypes = [ctypes.POINTER(Hook), ctypes.c_uint64,
                                               ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32)]
        cls.lib.hook_arm64_activate.argtypes = [ctypes.POINTER(Hook)]
        cls.lib.hook_arm64_restore.argtypes = [ctypes.POINTER(Hook)]

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def setUp(self):
        self.memory = mmap.mmap(-1, mmap.PAGESIZE, prot=mmap.PROT_READ | mmap.PROT_WRITE)
        self.address = ctypes.addressof(ctypes.c_char.from_buffer(self.memory))
        # Return a+b+1; the first four instructions are relocated unchanged.
        self.original = struct.pack("<8I", 0xd10043ff, 0xa9007bfd, 0x910003fd, 0x8b010000,
                                    0x91000400, 0xa9407bfd, 0x910043ff, 0xd65f03c0)
        self.memory[:len(self.original)] = self.original
        self.memory[256:264] = struct.pack("<2I", 0xd2800f60, 0xd65f03c0)  # return 123
        self.expected = (ctypes.c_uint32 * 4).from_buffer_copy(self.original[:16])
        self.hook = Hook()
        self.lib.fail_at(0)

    def tearDown(self):
        self.lib.fail_at(0)
        self.lib.hook_arm64_restore(ctypes.byref(self.hook))
        self.memory.close()

    def prepare(self):
        return self.lib.hook_arm64_prepare(ctypes.byref(self.hook), self.address,
                                           self.address + 256, self.expected)

    def execute(self, entry):
        vm = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        mapped = set()
        for address, length in [(self.address, mmap.PAGESIZE),
                                (self.hook.trampoline, self.hook.allocation_size)]:
            if not address: continue
            page = address & ~4095
            if page not in mapped:
                vm.mem_map(page, length); mapped.add(page)
            vm.mem_write(address, ctypes.string_at(address, length))
        vm.mem_map(0x100000, 0x20000)
        vm.reg_write(UC_ARM64_REG_SP, 0x110000)
        vm.reg_write(UC_ARM64_REG_X30, 0x100000)
        vm.reg_write(UC_ARM64_REG_X0, 17)
        vm.reg_write(UC_ARM64_REG_X1, 24)
        vm.emu_start(entry, 0x100000, count=100)
        self.assertEqual(vm.reg_read(UC_ARM64_REG_SP), 0x110000)
        return vm.reg_read(UC_ARM64_REG_X0)

    def test_detour_original_and_restore(self):
        self.assertEqual(self.prepare(), 0)
        self.assertEqual(ctypes.string_at(self.address,32),self.original)
        self.assertEqual(self.execute(self.hook.trampoline),42)
        self.assertEqual(self.lib.hook_arm64_activate(ctypes.byref(self.hook)),0)
        self.assertEqual(self.execute(self.address),123)
        self.assertEqual(self.execute(self.hook.trampoline),42)
        self.assertEqual(self.lib.hook_arm64_restore(ctypes.byref(self.hook)),0)
        self.assertEqual(ctypes.string_at(self.address,32),self.original)
        self.assertFalse(self.hook.trampoline)

    def test_pc_relative_at_every_stolen_position(self):
        for instruction in (0x90000000,0x10000000,0x14000001,0x94000001,0x54000020,
                            0x34000020,0x35000020,0x36000020,0x37000020,0x58000020):
            for position in range(4):
                words = [0xd503201f] * 4
                words[position] = instruction
                data = struct.pack("<4I",*words)
                ctypes.memmove(self.address,data,16)
                expected = (ctypes.c_uint32*4).from_buffer_copy(data)
                result=self.lib.hook_arm64_prepare(ctypes.byref(self.hook),self.address,
                                                   self.address+256,expected)
                self.assertEqual(result,-1,(instruction,position))
                self.assertFalse(self.hook.trampoline)
                self.assertEqual(ctypes.string_at(self.address,16),data)

    def test_mismatched_signature_leaves_target_untouched(self):
        self.expected[2] ^= 1
        self.assertEqual(self.prepare(),-1)
        self.assertEqual(ctypes.string_at(self.address,32),self.original)

    def test_allocation_protection_failure(self):
        self.lib.fail_at(1)
        self.assertEqual(self.prepare(),-1)
        self.assertFalse(self.hook.trampoline)
        self.assertEqual(ctypes.string_at(self.address,32),self.original)

    def test_target_permission_failure(self):
        self.assertEqual(self.prepare(),0)
        self.lib.fail_at(1)
        self.assertEqual(self.lib.hook_arm64_activate(ctypes.byref(self.hook)),-1)
        self.assertEqual(ctypes.string_at(self.address,32),self.original)

    def test_final_permission_failure_rolls_back(self):
        self.assertEqual(self.prepare(),0)
        self.lib.fail_at(2)
        self.assertEqual(self.lib.hook_arm64_activate(ctypes.byref(self.hook)),-1)
        self.assertFalse(self.hook.active)
        self.assertEqual(ctypes.string_at(self.address,32),self.original)
        self.assertEqual(self.execute(self.address),42)


if __name__ == "__main__":
    unittest.main()
