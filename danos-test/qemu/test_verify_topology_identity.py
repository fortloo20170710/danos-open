import hashlib
import shlex
import tempfile
import unittest
from pathlib import Path
from verify_topology_identity import validate


class IdentityTest(unittest.TestCase):
    def test_identity_and_rejections(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            artifact_dir = root / "artifacts with spaces"
            artifact_dir.mkdir()
            iso = artifact_dir / "test.iso"
            iso.write_bytes(b"ISO fixture, not a runtime result")
            commit = "a" * 40
            manifest = (f"git_commit={commit}\n"
                        f"danos_iso={shlex.quote(str(iso))}\n"
                        f"danos_iso_sha256={hashlib.sha256(iso.read_bytes()).hexdigest()}\n")
            log = f"DANOS-BUILD commit={commit} source_dirty=0 iso=test.iso utc=now\n"
            path = root / "run-manifest.env"
            serial = root / "danos.serial.log"
            path.write_text(manifest)
            serial.write_text(log)
            validate(root)
            for bad in ("", log + log, log.replace("source_dirty=0", "source_dirty=1"),
                        log.replace(commit, "b" * 40), log.replace("test.iso", "old.iso")):
                serial.write_text(bad)
                with self.subTest(log=bad), self.assertRaises(ValueError):
                    validate(root)
            serial.write_text(log)
            for bad in (manifest + f"git_commit={commit}\n",
                        manifest.replace(commit, "invalid"),
                        manifest.replace(shlex.quote(str(iso)), "relative.iso")):
                path.write_text(bad)
                with self.subTest(manifest=bad), self.assertRaises(ValueError):
                    validate(root)
            path.write_text(manifest)
            iso.write_bytes(b"changed content")
            with self.assertRaisesRegex(ValueError, "content"):
                validate(root)


if __name__ == "__main__":
    unittest.main()
