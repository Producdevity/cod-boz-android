import importlib.util
import os
from pathlib import Path
import struct
import tempfile
import unittest
from unittest import mock


SCRIPT = Path(__file__).resolve().parent.parent / "inspect-source-apk.py"
SPEC = importlib.util.spec_from_file_location("inspect_source_apk", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class InspectSourceApkTest(unittest.TestCase):
    def test_manifest_components_are_normalized(self):
        xml = """<?xml version="1.0"?>
        <manifest xmlns:android="http://schemas.android.com/apk/res/android"
          package="com.example.game" android:versionCode="7" android:versionName="1.2">
          <application>
            <activity android:name=".Main" />
            <service android:name="SyncService" android:exported="false" />
          </application>
        </manifest>"""

        report = MODULE.parse_manifest(xml)

        self.assertEqual("com.example.game", report["package"])
        self.assertEqual(7, report["version_code"])
        self.assertEqual(
            [
                {"type": "activity", "name": "com.example.game.Main"},
                {
                    "type": "service",
                    "name": "com.example.game.SyncService",
                    "exported": False,
                },
            ],
            report["components"],
        )

    def test_signing_metadata_is_parsed(self):
        report = MODULE.parse_signing(
            """Verified using v1 scheme (JAR signing): true
            Verified using v2 scheme (APK Signature Scheme v2): false
            Number of signers: 1
            Signer #1 certificate DN: CN=Publisher
            Signer #1 certificate SHA-256 digest: abcdef
            """
        )

        self.assertTrue(report["schemes"]["v1 scheme (JAR signing)"])
        self.assertFalse(report["schemes"]["v2 scheme (APK Signature Scheme v2)"])
        self.assertEqual("abcdef", report["certificate_sha256"])

    def test_arm32_elf_identity_is_parsed(self):
        header = bytearray(64)
        header[:6] = b"\x7fELF\x01\x01"
        struct.pack_into("<H", header, 18, 40)

        report = MODULE.parse_elf_header(bytes(header))

        self.assertEqual(32, report["class_bits"])
        self.assertEqual("little", report["endianness"])
        self.assertEqual("ARM", report["architecture"])

    def test_android_home_fallback_is_accepted(self):
        with mock.patch.dict(os.environ, {"ANDROID_HOME": "/tmp/android-sdk"}, clear=True):
            sdk = os.environ.get("ANDROID_SDK_ROOT") or os.environ.get("ANDROID_HOME")

        self.assertEqual("/tmp/android-sdk", sdk)

    def test_sdk_tool_selection_uses_package_revision(self):
        with tempfile.TemporaryDirectory() as directory:
            sdk = Path(directory)
            older = sdk / "build-tools" / "9.0.0"
            newer = sdk / "build-tools" / "10.0.0"
            older.mkdir(parents=True)
            newer.mkdir(parents=True)
            (older / "apksigner").touch()
            (newer / "apksigner").touch()
            (older / "source.properties").write_text("Pkg.Revision=9.0.0\n")
            (newer / "source.properties").write_text("Pkg.Revision=10.0.0\n")

            with mock.patch.object(MODULE.shutil, "which", return_value=None):
                selected = MODULE.command_path("apksigner", sdk)

            self.assertEqual(newer / "apksigner", selected)

    def test_profile_checks_include_manifest_components(self):
        report = {
            "source": {"sha256": "apk"},
            "manifest": {
                "package": "com.example.game",
                "version_code": 1,
                "version_name": "1",
                "min_sdk": 10,
                "target_sdk": 14,
                "components": [{"type": "activity", "name": "com.example.game.Main"}],
            },
            "signing": {"certificate_sha256": "certificate"},
            "native_libraries": [
                {"abi": "armeabi", "class_bits": 32, "architecture": "ARM"}
            ],
            "guest": {
                "arch": 268,
                "packed_size": 1,
                "packed_sha256": "packed",
                "unpacked_size": 2,
                "unpacked_sha256": "unpacked",
                "imports": ["first"],
                "resource_base_url": "https://example.invalid/",
                "resource_archives": [],
                "extension_hashes": [],
            },
        }
        import_descriptor = {
            "imports": ["first"],
            "source_inventory_sha256": MODULE.hashlib.sha256(b"first\n").hexdigest(),
        }
        profile = {
            "source_apk": {
                "sha256": "apk",
                "package": "com.example.game",
                "version_code": 1,
                "version_name": "1",
                "min_sdk": 10,
                "target_sdk": 14,
                "components": [{"type": "activity", "name": "com.example.game.Other"}],
                "signer_certificate_sha256": "certificate",
            },
            "guest": {
                "s3e_arch": 268,
                "packed_size": 1,
                "packed_sha256": "packed",
                "unpacked_size": 2,
                "unpacked_sha256": "unpacked",
            },
            "resources": {"base_url": "https://example.invalid/", "archives": []},
            "extensions": [],
        }

        with mock.patch.object(MODULE.json, "loads", return_value=import_descriptor):
            checks = MODULE.profile_checks(report, profile)

        self.assertFalse(checks["manifest_components"])


if __name__ == "__main__":
    unittest.main()
