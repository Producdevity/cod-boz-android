#!/usr/bin/env python3

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import xml.etree.ElementTree as ElementTree
import zipfile


ANDROID_NAMESPACE = "{http://schemas.android.com/apk/res/android}"
COMPONENT_TYPES = ("activity", "activity-alias", "service", "receiver", "provider")
REVISION_PATTERN = re.compile(r"^Pkg\.Revision\s*=\s*(\S+)\s*$", re.MULTILINE)


def revision_key(revision: str) -> tuple[int, int, int, int, int, str]:
    version, separator, qualifier = revision.partition("-")
    numbers = tuple(int(part) for part in version.split("."))
    if not numbers or len(numbers) > 4:
        raise ValueError(f"invalid Android SDK package revision: {revision}")
    padded = numbers + (0,) * (4 - len(numbers))
    return (*padded, 1 if not separator else 0, qualifier)


def tool_revision(path: Path, sdk_root: Path) -> tuple[int, int, int, int, int, str]:
    for parent in path.parents:
        if parent == sdk_root:
            break
        properties = parent / "source.properties"
        if not properties.is_file():
            continue
        match = REVISION_PATTERN.search(properties.read_text())
        if match:
            return revision_key(match.group(1))
    return (0, 0, 0, 0, 0, "")


def command_path(name: str, sdk_root: Path | None) -> Path:
    found = shutil.which(name)
    if found:
        return Path(found)
    if sdk_root:
        patterns = {
            "apkanalyzer": ["cmdline-tools/*/bin/apkanalyzer"],
            "apksigner": ["build-tools/*/apksigner"],
        }
        candidates: list[Path] = []
        for pattern in patterns[name]:
            candidates.extend(sdk_root.glob(pattern))
        if candidates:
            return max(candidates, key=lambda candidate: tool_revision(candidate, sdk_root))
    raise RuntimeError(f"{name} was not found; set ANDROID_SDK_ROOT or add it to PATH")


def run(command: list[str], accepted_codes: tuple[int, ...] = (0,)) -> str:
    completed = subprocess.run(command, capture_output=True, text=True, check=False)
    if completed.returncode not in accepted_codes:
        message = completed.stderr.strip() or completed.stdout.strip()
        raise RuntimeError(f"{' '.join(command[:2])} failed: {message}")
    return completed.stdout


def normalized_component_name(package: str, name: str) -> str:
    if name.startswith("."):
        return package + name
    if "." not in name:
        return f"{package}.{name}"
    return name


def parse_manifest(xml: str) -> dict:
    root = ElementTree.fromstring(xml)
    package = root.attrib["package"]
    application = root.find("application")
    components: list[dict] = []
    if application is not None:
        for component_type in COMPONENT_TYPES:
            for element in application.findall(component_type):
                name = element.attrib.get(ANDROID_NAMESPACE + "name")
                if not name:
                    continue
                component = {
                    "type": component_type,
                    "name": normalized_component_name(package, name),
                }
                exported = element.attrib.get(ANDROID_NAMESPACE + "exported")
                if exported is not None:
                    component["exported"] = exported == "true"
                components.append(component)
    return {
        "package": package,
        "version_code": int(root.attrib[ANDROID_NAMESPACE + "versionCode"]),
        "version_name": root.attrib[ANDROID_NAMESPACE + "versionName"],
        "components": components,
    }


def parse_signing(output: str) -> dict:
    result: dict = {"schemes": {}}
    for raw_line in output.splitlines():
        line = raw_line.strip()
        if line.startswith("Verified using ") and ": " in line:
            label, value = line.split(": ", 1)
            scheme = label.removeprefix("Verified using ").removesuffix(" scheme")
            result["schemes"][scheme] = value == "true"
        elif line.startswith("Number of signers: "):
            result["signer_count"] = int(line.rsplit(" ", 1)[1])
        elif line.startswith("Signer #1 certificate DN: "):
            result["certificate_dn"] = line.split(": ", 1)[1]
        elif line.startswith("Signer #1 certificate SHA-256 digest: "):
            result["certificate_sha256"] = line.rsplit(" ", 1)[1].lower()
    required = ("signer_count", "certificate_dn", "certificate_sha256")
    if any(field not in result for field in required):
        raise RuntimeError("apksigner output did not contain complete signer metadata")
    return result


def parse_elf_header(header: bytes) -> dict:
    if len(header) < 52 or header[:4] != b"\x7fELF":
        raise RuntimeError("native library has an invalid ELF header")
    elf_class = header[4]
    byte_order = header[5]
    if elf_class not in (1, 2) or byte_order not in (1, 2):
        raise RuntimeError("native library has an unsupported ELF identity")
    endian = "<" if byte_order == 1 else ">"
    machine = struct.unpack_from(endian + "H", header, 18)[0]
    machines = {40: "ARM", 62: "x86-64", 183: "AArch64"}
    return {
        "class_bits": 32 if elf_class == 1 else 64,
        "endianness": "little" if byte_order == 1 else "big",
        "machine": machine,
        "architecture": machines.get(machine, "unknown"),
    }


def native_libraries(apk_path: Path) -> list[dict]:
    libraries: list[dict] = []
    with zipfile.ZipFile(apk_path) as apk:
        for name in sorted(apk.namelist()):
            parts = name.split("/")
            if len(parts) != 3 or parts[0] != "lib" or not parts[2].endswith(".so"):
                continue
            with apk.open(name) as library:
                elf = parse_elf_header(library.read(64))
            libraries.append({"path": name, "abi": parts[1], **elf})
    return libraries


def manifest_report(apkanalyzer: Path, apk_path: Path) -> dict:
    prefix = [str(apkanalyzer), "manifest"]
    manifest = parse_manifest(run(prefix + ["print", str(apk_path)]))
    manifest["min_sdk"] = int(run(prefix + ["min-sdk", str(apk_path)]).strip())
    manifest["target_sdk"] = int(run(prefix + ["target-sdk", str(apk_path)]).strip())
    return manifest


def native_report(inspector: Path, apk_path: Path) -> dict:
    output = run([str(inspector), str(apk_path)], accepted_codes=(0, 1))
    try:
        return json.loads(output)
    except json.JSONDecodeError as error:
        raise RuntimeError(f"native inspector returned invalid JSON: {error}") from error


def profile_checks(report: dict, profile: dict) -> dict:
    source = profile["source_apk"]
    guest = profile["guest"]
    resources = profile["resources"]
    expected_extensions = {item["hash"] for item in profile["extensions"]}
    found_extensions = {item["hash"] for item in report["guest"]["extension_hashes"]}
    expected_archives = {
        (item["name"], item["size"], item["sha256"]) for item in resources["archives"]
    }
    found_archives = {
        (item["name"], item["size"], item["sha256"])
        for item in report["guest"]["resource_archives"]
    }
    libraries = report["native_libraries"]
    import_descriptor_path = Path(__file__).resolve().parent.parent / "native/bridge/imports.json"
    import_descriptor = json.loads(import_descriptor_path.read_text())
    found_imports = report["guest"]["imports"]
    found_import_digest = hashlib.sha256(
        ("\n".join(found_imports) + "\n").encode()
    ).hexdigest()
    checks = {
        "apk_sha256": report["source"]["sha256"] == source["sha256"],
        "manifest_package": report["manifest"]["package"] == source["package"],
        "manifest_version_code": report["manifest"]["version_code"] == source["version_code"],
        "manifest_version_name": report["manifest"]["version_name"] == source["version_name"],
        "manifest_min_sdk": report["manifest"]["min_sdk"] == source["min_sdk"],
        "manifest_target_sdk": report["manifest"]["target_sdk"] == source["target_sdk"],
        "manifest_components": report["manifest"]["components"] == source["components"],
        "signer_certificate": (
            report["signing"]["certificate_sha256"] == source["signer_certificate_sha256"]
        ),
        "native_libraries_arm32": bool(libraries)
        and all(
            item["abi"] == "armeabi"
            and item["class_bits"] == 32
            and item["architecture"] == "ARM"
            for item in libraries
        ),
        "guest_architecture": report["guest"]["arch"] == guest["s3e_arch"],
        "guest_payload": (
            report["guest"]["packed_size"] == guest["packed_size"]
            and report["guest"]["packed_sha256"] == guest["packed_sha256"]
            and report["guest"]["unpacked_size"] == guest["unpacked_size"]
            and report["guest"]["unpacked_sha256"] == guest["unpacked_sha256"]
        ),
        "guest_import_inventory": (
            found_imports == import_descriptor["imports"]
            and found_import_digest == import_descriptor["source_inventory_sha256"]
        ),
        "resource_base_url": report["guest"]["resource_base_url"] == resources["base_url"],
        "resource_archives": found_archives == expected_archives
        and all(
            item["guest_string_occurrences"] > 0
            for item in report["guest"]["resource_archives"]
        ),
        "extension_hashes": found_extensions == expected_extensions,
    }
    return checks


def create_report(
    apk_path: Path,
    profile_path: Path,
    inspector: Path,
    apkanalyzer: Path,
    apksigner: Path,
) -> dict:
    profile = json.loads(profile_path.read_text())
    native = native_report(inspector, apk_path)
    report = {
        "schema_version": 1,
        "source": {
            "path": str(apk_path.resolve()),
            "size": native["size"],
            "sha256": native["sha256"],
        },
        "manifest": manifest_report(apkanalyzer, apk_path),
        "signing": parse_signing(
            run([str(apksigner), "verify", "--verbose", "--print-certs", str(apk_path)])
        ),
        "native_libraries": native_libraries(apk_path),
        "zip": {
            "entry_count": native["entry_count"],
            "asset_count": native["asset_count"],
        },
        "guest": native["s3e"],
    }
    report["profile_checks"] = profile_checks(report, profile)
    report["valid_supported_source"] = all(report["profile_checks"].values())
    return report


def arguments() -> argparse.Namespace:
    repository = Path(__file__).resolve().parent.parent
    inspector_candidates = (
        repository / "build" / "host-tests" / "apk_inspect",
        repository / ".build" / "host" / "apk_inspect",
    )
    default_inspector = next(
        (candidate for candidate in inspector_candidates if candidate.is_file()),
        inspector_candidates[0],
    )
    parser = argparse.ArgumentParser(
        description="Inspect the supported source APK without executing game code."
    )
    parser.add_argument("apk", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument(
        "--profile", type=Path, default=repository / "config" / "source-profile.json"
    )
    parser.add_argument(
        "--native-inspector", type=Path, default=default_inspector
    )
    return parser.parse_args()


def main() -> int:
    options = arguments()
    if not options.apk.is_file():
        raise RuntimeError(f"APK does not exist: {options.apk}")
    if not options.native_inspector.is_file():
        raise RuntimeError(
            "native inspector is not built; run cmake -S native -B build/host-tests "
            "&& cmake --build build/host-tests --parallel"
        )
    sdk = os.environ.get("ANDROID_SDK_ROOT") or os.environ.get("ANDROID_HOME")
    sdk_root = Path(sdk) if sdk else None
    report = create_report(
        options.apk,
        options.profile,
        options.native_inspector,
        command_path("apkanalyzer", sdk_root),
        command_path("apksigner", sdk_root),
    )
    output = json.dumps(report, indent=2) + "\n"
    if options.output:
        options.output.parent.mkdir(parents=True, exist_ok=True)
        options.output.write_text(output)
    else:
        sys.stdout.write(output)
    return 0 if report["valid_supported_source"] else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (KeyError, OSError, RuntimeError, TypeError, ValueError, zipfile.BadZipFile) as error:
        print(f"inspect-source-apk: {error}", file=sys.stderr)
        raise SystemExit(2)
