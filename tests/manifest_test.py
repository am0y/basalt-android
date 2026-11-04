import importlib.util
from pathlib import Path
import struct
import sys
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("builder", ROOT / "work/scripts/build_phone_apk.py")
builder = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = builder
spec.loader.exec_module(builder)

def inspect(data):
    assert struct.unpack_from("<I", data, 4)[0] == len(data)
    strings, permissions, attributes = [], [], []
    cursor = 8
    while cursor < len(data):
        kind, _, size = struct.unpack_from("<HHI", data, cursor)
        assert size > 0 and cursor + size <= len(data)
        chunk = data[cursor:cursor + size]
        if kind == 1:
            strings = builder.parse_string_pool(chunk)
        elif kind == 0x102:
            tag, start, stride, count = struct.unpack_from("<IHHH", chunk, 20)
            for i in range(count):
                values = struct.unpack_from("<IIIHBBI", chunk, 16 + start + i * stride)
                namespace, name, raw, value_size, _, value_type, value = values
                assert value_size == 8
                attributes.append((strings[tag], strings[name], value_type, value))
                if strings[tag] == "uses-permission" and strings[name] == "name":
                    assert value_type == 3
                    permissions.append(strings[value])
        cursor += size
    return strings, permissions, attributes

class ManifestTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not (ROOT / "work/base.apk").is_file():
            raise unittest.SkipTest("local base.apk is required for manifest checks")
        with zipfile.ZipFile(ROOT / "work/base.apk") as archive:
            cls.original = archive.read("AndroidManifest.xml")
            cls.network = archive.read("res/xml/network_security_config.xml")

    def test_permission_added_once_and_string_indices_preserved(self):
        changed = builder.public_storage_manifest(self.original)
        old_strings, old_permissions, _ = inspect(self.original)
        strings, permissions, _ = inspect(changed)
        self.assertEqual(strings[:len(old_strings)], old_strings)
        self.assertEqual(permissions.count("android.permission.MANAGE_EXTERNAL_STORAGE"), 1)
        self.assertEqual(len(permissions), len(old_permissions) + 1)
        self.assertEqual(builder.public_storage_manifest(changed), changed)

    def test_standalone_values_are_well_formed(self):
        _, permissions, attrs = inspect(builder.standalone_manifest(self.original))
        self.assertIn("android.permission.MANAGE_EXTERNAL_STORAGE", permissions)
        self.assertTrue(any(tag == "meta-data" and name == "value" and kind == 0x12 and value == 0
                            for tag, name, kind, value in attrs))
        self.assertEqual([value for tag,name,kind,value in attrs if tag == "uses-sdk" and name == "minSdkVersion"], [29])
        strings, _, _ = inspect(builder.standalone_manifest(self.original))
        self.assertTrue(any(tag == "manifest" and name == "requiredSplitTypes" and kind == 3 and strings[value] == ""
                            for tag,name,kind,value in attrs))

    def test_http_allowed_without_changing_tls_trust(self):
        old_strings, _, old_attrs = inspect(self.network)
        changed = builder.http_network_config(self.network)
        strings, _, attrs = inspect(changed)
        self.assertEqual(strings[:len(old_strings)], old_strings)
        self.assertEqual([a for a in attrs if a[0] == "certificates"],
                         [a for a in old_attrs if a[0] == "certificates"])
        self.assertEqual([a for a in attrs if a[1] == "cleartextTrafficPermitted"],
                         [("base-config", "cleartextTrafficPermitted", 0x12, 0xffffffff)])
        self.assertEqual(builder.http_network_config(changed), changed)

if __name__ == "__main__":
    unittest.main()
