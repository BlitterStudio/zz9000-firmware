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


if __name__ == "__main__":
    unittest.main()
