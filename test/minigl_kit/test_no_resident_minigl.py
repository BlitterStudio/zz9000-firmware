import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
CHECK = ROOT / "util" / "check_no_resident_minigl.sh"


class NoResidentMiniGLTests(unittest.TestCase):
    def gate(self, makefile, link_map):
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            elf = directory / "ZZ9000OS.elf"
            elf.write_bytes(b"ELF")
            map_path = directory / "ZZ9000OS.map"
            map_path.write_text(link_map, encoding="utf-8")
            makefile_path = directory / "Makefile"
            makefile_path.write_text(makefile, encoding="utf-8")
            return subprocess.run(
                ["sh", str(CHECK), str(elf), str(map_path), str(makefile_path)],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )

    def test_accepts_clean_firmware_link(self):
        result = self.gate("C_SRCS = src/main.c\n", "build/main.o\n")
        self.assertEqual(0, result.returncode, result.stderr)

    def test_rejects_private_front_end_dependency(self):
        result = self.gate("MINIGL_LIB_DIR ?= ../../../private\n", "build/main.o\n")
        self.assertNotEqual(0, result.returncode)
        self.assertIn("private MiniGL", result.stderr)

    def test_rejects_resident_minigl_object(self):
        result = self.gate("C_SRCS = src/main.c\n", "build/mw_service.o\n")
        self.assertNotEqual(0, result.returncode)
        self.assertIn("resident MiniGL", result.stderr)


if __name__ == "__main__":
    unittest.main()
