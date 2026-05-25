import json
import os
import shutil
import subprocess
import tempfile
import time
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
HOOK = ROOT / "integrations" / "codex" / "codex_status_hook.ps1"


class CodexStatusHookTests(unittest.TestCase):
    def test_hook_payload_includes_model_reasoning_window_and_quota_snapshot(self):
        powershell = shutil.which("powershell.exe") or shutil.which("powershell")
        if not powershell:
            self.skipTest("powershell is required")

        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            codex_home = tmp_path / ".codex"
            sessions_dir = codex_home / "sessions" / "2026" / "05" / "25"
            sessions_dir.mkdir(parents=True)
            (codex_home / "config.toml").write_text(
                '\n'.join(
                    [
                        'model = "gpt-5.5"',
                        'model_reasoning_effort = "medium"',
                        'service_tier = "default"',
                    ]
                ),
                encoding="utf-8",
            )
            session_file = sessions_dir / "rollout-test.jsonl"
            session_file.write_text(
                json.dumps(
                    {
                        "timestamp": "2026-05-25T11:18:20.865Z",
                        "type": "event_msg",
                        "payload": {
                            "type": "token_count",
                            "info": {
                                "total_token_usage": {
                                    "input_tokens": 13048203,
                                    "cached_input_tokens": 12403456,
                                    "output_tokens": 55746,
                                    "reasoning_output_tokens": 23279,
                                    "total_tokens": 13103949,
                                },
                                "last_token_usage": {"total_tokens": 215455},
                                "model_context_window": 258400,
                            },
                            "rate_limits": {
                                "primary": {"used_percent": 18.0},
                                "secondary": {"used_percent": 22.0},
                            },
                        },
                    }
                )
                + "\n",
                encoding="utf-8",
            )

            capture_path = tmp_path / "captured.json"
            fake_sender = tmp_path / "nomi-send.cmd"
            fake_sender.write_text(
                "@echo off\r\n"
                "copy /Y \"%~3\" \"%NOMI_CAPTURE_PATH%\" >NUL\r\n"
                "exit /b 0\r\n",
                encoding="utf-8",
            )
            hold_script = tmp_path / "hold-session-open.ps1"
            ready_path = tmp_path / "session-ready.txt"
            hold_script.write_text(
                "$path = $args[0]\n"
                "$ready = $args[1]\n"
                "$share = [System.IO.FileShare]::ReadWrite\n"
                "$stream = [System.IO.File]::Open($path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Write, $share)\n"
                "[System.IO.File]::WriteAllText($ready, 'ready')\n"
                "Start-Sleep -Seconds 20\n"
                "$stream.Dispose()\n",
                encoding="utf-8",
            )

            env = os.environ.copy()
            env["CODEX_HOME"] = str(codex_home)
            env["NOMI_SEND_PATH"] = str(fake_sender)
            env["NOMI_CAPTURE_PATH"] = str(capture_path)

            holder = subprocess.Popen(
                [
                    powershell,
                    "-NoProfile",
                    "-ExecutionPolicy",
                    "Bypass",
                    "-File",
                    str(hold_script),
                    str(session_file),
                    str(ready_path),
                ],
                cwd=ROOT,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            try:
                for _ in range(50):
                    if ready_path.exists():
                        break
                    time.sleep(0.1)
                self.assertTrue(ready_path.exists(), "session file holder did not start")

                proc = subprocess.run(
                    [
                        powershell,
                        "-NoProfile",
                        "-ExecutionPolicy",
                        "Bypass",
                        "-File",
                        str(HOOK),
                        "UserPromptSubmit",
                    ],
                    cwd=ROOT,
                    input='{"prompt":"hook payload smoke test"}',
                    text=True,
                    capture_output=True,
                    env=env,
                    timeout=10,
                )
            finally:
                holder.terminate()
                try:
                    holder.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    holder.kill()

            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertIn('"continue":true', proc.stdout)

            for _ in range(50):
                if capture_path.exists():
                    break
                time.sleep(0.1)

            self.assertTrue(capture_path.exists(), "fake nomi-send did not capture a payload")
            payload = json.loads(capture_path.read_text(encoding="utf-8"))

            self.assertEqual(payload["model"], "gpt-5.5")
            self.assertEqual(payload["effort"], "medium")
            self.assertEqual(payload["tier"], "default")
            self.assertEqual(payload["context_pct"], 83)
            self.assertEqual(payload["used_tokens_k"], 700)
            self.assertEqual(payload["quota"]["five_hour_left"], 82)
            self.assertEqual(payload["quota"]["weekly_left"], 78)
            self.assertEqual(payload["prompt"], "hook payload smoke test")


if __name__ == "__main__":
    unittest.main()
