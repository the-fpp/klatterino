from pathlib import Path
import unittest


class StreamlinkDiscoveryTests(unittest.TestCase):
    def test_pinned_streamlink_loads_rumble_plugin(self):
        try:
            from streamlink import Streamlink
        except ImportError:
            self.skipTest("Streamlink is supplied by the Nix validation check")
        plugin_dir = Path(__file__).resolve().parents[2] / "streamlink-plugins"
        session = Streamlink()
        self.assertTrue(session.plugins.load_path(plugin_dir))
        name, plugin, resolved = session.resolve_url_no_redirect("https://rumble.com/c/Fixture")
        self.assertEqual("rumble", name)
        self.assertEqual("https://rumble.com/c/Fixture", resolved)
        self.assertEqual("Rumble", plugin.__name__)


if __name__ == "__main__":
    unittest.main()
