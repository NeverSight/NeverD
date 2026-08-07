import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CMAKE_HELPERS = ROOT / "cmake" / "AddNeverD.cmake"
WORKFLOW = ROOT / ".github" / "workflows" / "ci.yml"


class CiConfigurationTests(unittest.TestCase):
    def test_google_test_discovery_is_serial_and_bounded(self):
        source = CMAKE_HELPERS.read_text(encoding="utf-8")
        self.assertIn("DISCOVERY_MODE PRE_TEST", source)
        self.assertIn("DISCOVERY_TIMEOUT 120", source)
        self.assertNotIn("DISCOVERY_MODE POST_BUILD", source)
        self.assertNotIn("DISCOVERY_TIMEOUT -1", source)
