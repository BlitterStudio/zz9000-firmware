import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
CHECK = ROOT / "util" / "check_minigl_kit.py"
FIXTURES = pathlib.Path(__file__).resolve().parent


class MiniGLKitManifestTests(unittest.TestCase):
    def check(self, *arguments):
        return subprocess.run(
            [sys.executable, str(CHECK), *map(str, arguments)],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )

    def test_fresh_standard_and_rasterix_kit_matrix(self):
        result = self.check("matrix", FIXTURES / "matrix.json")
        self.assertEqual(0, result.returncode, result.stderr)

    def test_rejects_wrong_wire_version(self):
        manifest = json.loads((FIXTURES / "standard.json").read_text(encoding="utf-8"))
        manifest["module"]["required"]["wire_version"] = 8
        with tempfile.TemporaryDirectory() as temporary:
            path = pathlib.Path(temporary) / "bad-wire.json"
            path.write_text(json.dumps(manifest), encoding="utf-8")
            result = self.check("validate", path)
        self.assertNotEqual(0, result.returncode)
        self.assertIn("wire_version", result.stderr)

    def test_rejects_wrong_module_hash(self):
        module = b"MINIGL.ZZM test image"
        manifest = json.loads((FIXTURES / "standard.json").read_text(encoding="utf-8"))
        with tempfile.TemporaryDirectory() as temporary:
            path = pathlib.Path(temporary)
            manifest_path = path / "MINIGL.KIT.json"
            module_path = path / "MINIGL.ZZM"
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            module_path.write_bytes(module)
            result = self.check("validate", manifest_path, "--module", module_path)
        self.assertNotEqual(0, result.returncode)
        self.assertIn("SHA-256", result.stderr)

    def test_staging_preserves_a_verified_module_and_manifest(self):
        module = b"MINIGL.ZZM test image"
        source = b"corresponding source"
        manifest = json.loads((FIXTURES / "standard.json").read_text(encoding="utf-8"))
        manifest["module"]["zzm3_sha256"] = hashlib.sha256(module).hexdigest()
        manifest["module"]["source_tarball_sha256"] = hashlib.sha256(source).hexdigest()
        with tempfile.TemporaryDirectory() as temporary:
            kit = pathlib.Path(temporary) / "kit"
            destination = pathlib.Path(temporary) / "release"
            kit.mkdir()
            (kit / "MINIGL.ZZM").write_bytes(module)
            (kit / "MINIGL.KIT.json").write_text(json.dumps(manifest), encoding="utf-8")
            (kit / "MINIGL-source-test.tar.gz").write_bytes(source)
            result = subprocess.run(
                ["sh", "util/stage_minigl_kit.sh", str(kit), str(destination),
                 "--require-source"],
                cwd=ROOT,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            self.assertEqual(0, result.returncode, result.stderr)
            self.assertEqual(module, (destination / "ZZ9K/MINIGL.ZZM").read_bytes())
            self.assertTrue((destination / "MINIGL-source-test.tar.gz").is_file())


if __name__ == "__main__":
    unittest.main()
