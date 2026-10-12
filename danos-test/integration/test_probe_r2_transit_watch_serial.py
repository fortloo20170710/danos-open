import unittest

from probe_r2_transit_watch_serial import remote_script


class TransitWatchSerialTests(unittest.TestCase):
    def test_start_and_collect_remote_code_compiles(self):
        for action in ("start", "collect"):
            marker, script = remote_script(
                action,
                "/tmp/danos-r2-transit-watch-0123456789abcdef0123456789abcdef.json",
                90,
            )
            self.assertTrue(marker.startswith("DANOS_R2_TRANSIT_WATCH_"))
            if action == "start":
                self.assertIn("30.30.30.2", script)
                self.assertNotIn("Path(None", script)
                self.assertRegex(script, r"danos-r2-transit-watch-[0-9a-f]{32}\.json")

    def test_collect_rejects_untrusted_path(self):
        with self.assertRaisesRegex(ValueError, "invalid bounded worker token"):
            remote_script("collect", "/tmp/other.json", 90)


if __name__ == "__main__":
    unittest.main()
