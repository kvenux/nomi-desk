import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
XTEINK_SRC = ROOT / "firmware" / "xteink" / "src"


def read_source(name: str) -> str:
    return (XTEINK_SRC / name).read_text(encoding="utf-8")


class XteinkNomiStatusStaticTests(unittest.TestCase):
    def test_xteink_advertises_nomi_v1_ble_contract(self):
        bridge = read_source("CodexBleBridge.cpp")

        self.assertIn('"Nomi XTEINK"', bridge)
        self.assertIn('"f4f688c2-613e-56a5-b115-d19a99d1b463"', bridge)
        self.assertIn('"74879a99-7275-5b33-9665-51519f328fa5"', bridge)
        self.assertIn('"830ac719-8dea-541c-8d18-5e8de4cd83dd"', bridge)
        self.assertIn('"485d9275-a3ad-516d-a524-e284f0aafdb1"', bridge)
        self.assertIn(
            '"{\\"protocol\\":\\"nomi-agent-display\\",\\"version\\":1,'
            '\\"device\\":\\"xteink\\",\\"width\\":480,\\"height\\":800}"',
            bridge,
        )

    def test_xteink_status_accepts_nomi_payload_fields(self):
        status = read_source("CodexStatus.cpp")

        self.assertIn('obj["protocol"]', status)
        self.assertIn("nomi-agent-display", status)
        self.assertIn('obj["version"]', status)
        self.assertIn('assignString(obj, "source"', status)
        self.assertIn('obj["model"]', status)
        self.assertIn('obj["effort"]', status)
        self.assertIn('obj["tier"]', status)
        self.assertIn("ApplyNomiModelLine", status)
        self.assertIn('assignString(obj, "session"', status)
        self.assertIn('assignString(obj, "event"', status)
        self.assertIn('assignString(obj, "prompt"', status)
        self.assertIn('obj["used_tokens_k"]', status)
        self.assertIn('quota["five_hour_left"]', status)
        self.assertIn('quota["weekly_left"]', status)

    def test_xteink_status_updates_use_fast_refresh_after_initial_full_refresh(self):
        main = read_source("main.cpp")

        self.assertIn("static bool firstPaint = true;", main)
        self.assertIn("firstPaint ? HalDisplay::FULL_REFRESH : refreshMode", main)
        self.assertIn("draw(HalDisplay::FAST_REFRESH)", main)

    def test_xteink_buttons_are_not_mapped_to_host_events_yet(self):
        main = read_source("main.cpp")
        handle_buttons = re.search(
            r"static void handleButtons\(\) \{(?P<body>.*?)\n\}",
            main,
            re.DOTALL,
        )

        self.assertIsNotNone(handle_buttons, "handleButtons() not found")
        self.assertNotIn("notifyButton", handle_buttons.group("body"))


if __name__ == "__main__":
    unittest.main()
