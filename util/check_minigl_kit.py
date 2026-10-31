#!/usr/bin/env python3
"""Validate a MiniGL kit manifest and its firmware/module compatibility."""
import argparse
import hashlib
import json
import pathlib
import sys

SCHEMA = "zz9000-minigl-kit/v1"
MODULE_ID_MINIGL = 4
WIRE_VERSION = 9
HARDWARE_REQUIREMENTS = {
    "CPU_ONLY",
    "RASTERIX_EF_OPTIONAL",
    "RASTERIX_EF_REQUIRED",
}


def fail(message):
    raise ValueError(message)


def object_field(value, name):
    if not isinstance(value, dict):
        fail(f"{name} must be an object")
    return value


def fields(value, name, required, optional=()):
    value = object_field(value, name)
    missing = set(required) - value.keys()
    unknown = value.keys() - set(required) - set(optional)
    if missing:
        fail(f"{name} missing {', '.join(sorted(missing))}")
    if unknown:
        fail(f"{name} has unknown {', '.join(sorted(unknown))}")
    return value


def integer(value, name):
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        fail(f"{name} must be a non-negative integer")


def version(value, name):
    value = fields(value, name, ("major", "minor"))
    integer(value["major"], f"{name}.major")
    integer(value["minor"], f"{name}.minor")
    return value


def sha256(value, name):
    if not isinstance(value, str) or len(value) != 64:
        fail(f"{name} must be a 64-character lowercase SHA-256")
    try:
        int(value, 16)
    except ValueError:
        fail(f"{name} must be a 64-character lowercase SHA-256")
    if value != value.lower():
        fail(f"{name} must be a 64-character lowercase SHA-256")


def load_manifest(path):
    try:
        loaded = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"{path}: {error}") from error
    return loaded


def validate_manifest(manifest):
    manifest = fields(manifest, "manifest", ("schema", "firmware", "module", "bitstream", "library"))
    if manifest["schema"] != SCHEMA:
        fail(f"schema must be {SCHEMA}")

    firmware = fields(manifest["firmware"], "firmware", ("build_id", "module_abi"))
    if not isinstance(firmware["build_id"], str) or not firmware["build_id"]:
        fail("firmware.build_id must be a non-empty string")
    module_abi = version(firmware["module_abi"], "firmware.module_abi")
    if module_abi["major"] != 3:
        fail("firmware.module_abi.major must be 3")

    module = fields(manifest["module"], "module", ("id", "version", "zzm3_sha256", "required"), ("source_tarball_sha256",))
    if module["id"] != MODULE_ID_MINIGL:
        fail("module.id must be 4 (MINIGL)")
    if not isinstance(module["version"], str) or not module["version"]:
        fail("module.version must be a non-empty string")
    sha256(module["zzm3_sha256"], "module.zzm3_sha256")
    if "source_tarball_sha256" in module:
        sha256(module["source_tarball_sha256"], "module.source_tarball_sha256")
    required = fields(module["required"], "module.required", ("wire_version", "transport_descriptor_major", "rasterix_csr_abi", "hardware_requirement"))
    if required["wire_version"] != WIRE_VERSION:
        fail("module.required.wire_version must be 9")
    integer(required["transport_descriptor_major"], "module.required.transport_descriptor_major")
    if not required["transport_descriptor_major"]:
        fail("module.required.transport_descriptor_major must be at least 1")
    csr_range = fields(required["rasterix_csr_abi"], "module.required.rasterix_csr_abi", ("major", "min_minor", "max_minor"))
    for key in ("major", "min_minor", "max_minor"):
        integer(csr_range[key], f"module.required.rasterix_csr_abi.{key}")
    if csr_range["min_minor"] > csr_range["max_minor"]:
        fail("module.required.rasterix_csr_abi.min_minor exceeds max_minor")
    if required["hardware_requirement"] not in HARDWARE_REQUIREMENTS:
        fail("module.required.hardware_requirement is invalid")

    bitstream = fields(manifest["bitstream"], "bitstream", ("flavour", "rasterix_ef", "rasterix_csr_abi"))
    if not isinstance(bitstream["flavour"], str) or not bitstream["flavour"]:
        fail("bitstream.flavour must be a non-empty string")
    if not isinstance(bitstream["rasterix_ef"], bool):
        fail("bitstream.rasterix_ef must be a boolean")
    if bitstream["rasterix_ef"]:
        version(bitstream["rasterix_csr_abi"], "bitstream.rasterix_csr_abi")
    elif bitstream["rasterix_csr_abi"] is not None:
        fail("a non-RasterIX bitstream must have null rasterix_csr_abi")

    library = fields(manifest["library"], "library", ("version", "expected_capability_bits"))
    if not isinstance(library["version"], str) or not library["version"]:
        fail("library.version must be a non-empty string")
    integer(library["expected_capability_bits"], "library.expected_capability_bits")
    return manifest


def compatibility_error(manifest):
    required = manifest["module"]["required"]
    image = manifest["bitstream"]
    if required["hardware_requirement"] == "RASTERIX_EF_REQUIRED" and not image["rasterix_ef"]:
        return "RasterIX-EF-required module is paired with a non-RasterIX bitstream"
    if not image["rasterix_ef"]:
        return None
    provided = image["rasterix_csr_abi"]
    requested = required["rasterix_csr_abi"]
    if provided["major"] != requested["major"] or not requested["min_minor"] <= provided["minor"] <= requested["max_minor"]:
        return "RasterIX CSR ABI is outside the module requirement range"
    return None


def command_validate(args):
    manifest = validate_manifest(load_manifest(args.manifest))
    error = compatibility_error(manifest)
    if error:
        fail(error)
    if args.module is not None:
        digest = hashlib.sha256(args.module.read_bytes()).hexdigest()
        if digest != manifest["module"]["zzm3_sha256"]:
            fail(f"{args.module} SHA-256 does not match module.zzm3_sha256")
    if args.source_tarball is not None:
        expected = manifest["module"].get("source_tarball_sha256")
        if expected is None:
            fail("manifest does not declare module.source_tarball_sha256")
        digest = hashlib.sha256(args.source_tarball.read_bytes()).hexdigest()
        if digest != expected:
            fail(f"{args.source_tarball} SHA-256 does not match module.source_tarball_sha256")


def command_matrix(args):
    cases = load_manifest(args.matrix)
    if not isinstance(cases, list) or not cases:
        fail("matrix must be a non-empty JSON array")
    for case in cases:
        case = fields(case, "matrix case", ("manifest", "compatible"))
        if not isinstance(case["compatible"], bool):
            fail("matrix case compatible must be a boolean")
        manifest = validate_manifest(load_manifest(args.matrix.parent / case["manifest"]))
        compatible = compatibility_error(manifest) is None
        if compatible != case["compatible"]:
            fail(f"matrix case {case['manifest']} expected compatible={case['compatible']}, got {compatible}")


def main():
    parser = argparse.ArgumentParser()
    subcommands = parser.add_subparsers(dest="command", required=True)
    validate = subcommands.add_parser("validate")
    validate.add_argument("manifest", type=pathlib.Path)
    validate.add_argument("--module", type=pathlib.Path)
    validate.add_argument("--source-tarball", type=pathlib.Path)
    matrix = subcommands.add_parser("matrix")
    matrix.add_argument("matrix", type=pathlib.Path)
    args = parser.parse_args()
    try:
        if args.command == "validate":
            command_validate(args)
        else:
            command_matrix(args)
    except ValueError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
