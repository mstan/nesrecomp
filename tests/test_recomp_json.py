"""Execute the project metadata command without a game or a compiler."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

TOOL = Path(__file__).resolve().parents[1] / "tools/write_recomp_json.py"


class ProjectMetadataTest(unittest.TestCase):
    def invoke(self, out, *args):
        return subprocess.run([sys.executable, str(TOOL), str(out), "--game",
                               'Fixture Quest: "Another" World', *args],
                              capture_output=True, text=True, encoding="utf-8",
                              creationflags=0x08000000 if os.name == "nt" else 0)

    def test_minimal_identity_does_not_invent_release_or_launcher_support(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / ".recomp.json"
            result = self.invoke(out)
            self.assertEqual(result.returncode, 0, result.stderr)
            doc = json.loads(out.read_text())
            self.assertEqual(doc, {"$schema": "https://recomp.fyi/schema/v1.json",
                "game": 'Fixture Quest: "Another" World', "system": "NES",
                "type": "recomp", "toolchain": "nesrecomp", "status": "exploring"})

    def test_owned_input_is_hashed_without_copying_data_or_local_path(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            rom = root / "synthetic private input.bin"
            data = b"Redistributable identity fixture\x00\x01"
            rom.write_bytes(data)
            out = root / ".recomp.json"
            result = self.invoke(out, "--rom", str(rom), "--region", "Europe")
            self.assertEqual(result.returncode, 0, result.stderr)
            doc = json.loads(out.read_text())
            self.assertEqual(doc["original"], {"region": "Europe", "sha1": hashlib.sha1(data).hexdigest()})
            self.assertNotIn(str(rom), out.read_text())
            self.assertEqual(rom.read_bytes(), data)

    def test_maintainer_fields_and_original_values_survive_fill_missing(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / ".recomp.json"
            existing = {"game": "Published title", "status": "released",
                        "maintainers": [{"name": "A developer"}], "help": ["Audio"],
                        "original": {"region": "Japan", "revision": "1.1"},
                        "x-project-detail": {"keep": True}}
            out.write_text(json.dumps(existing))
            result = self.invoke(out, "--region", "USA", "--sha1", "AB" * 20)
            self.assertEqual(result.returncode, 0, result.stderr)
            doc = json.loads(out.read_text())
            for key in existing:
                if key != "original": self.assertEqual(doc[key], existing[key])
            self.assertEqual(doc["original"], {**existing["original"], "sha1": "ab" * 20})

    def test_multiple_original_releases_are_not_replaced(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / ".recomp.json"
            releases = [{"region": "Japan"}, {"region": "USA"}]
            out.write_text(json.dumps({"original": releases}))
            result = self.invoke(out, "--region", "Europe")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(out.read_text())["original"], releases)

    def test_invalid_existing_documents_are_rejected_unchanged(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / ".recomp.json"
            for original in [b"{broken", b"[1, 2]"]:
                out.write_bytes(original)
                self.assertEqual(self.invoke(out).returncode, 1)
                self.assertEqual(out.read_bytes(), original)

    def test_repeated_command_preserves_already_complete_file_bytes(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / ".recomp.json"
            self.assertEqual(self.invoke(out).returncode, 0)
            compact = json.dumps(json.loads(out.read_text()), separators=(",", ":")).encode()
            out.write_bytes(compact)
            self.assertEqual(self.invoke(out).returncode, 0)
            self.assertEqual(out.read_bytes(), compact)

    def test_invalid_probe_hash_does_not_create_metadata(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / ".recomp.json"
            self.assertNotEqual(self.invoke(out, "--sha1", "not-a-hash").returncode, 0)
            self.assertFalse(out.exists())


if __name__ == "__main__":
    unittest.main()
