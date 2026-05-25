#!/usr/bin/env python3
"""Send Codex status-line data to the XTEINK BLE dashboard."""

from __future__ import annotations

import argparse
import asyncio
import json
import sys
from pathlib import Path

SERVICE_UUID = "6f30d210-2f6d-4a8c-9f78-42d8d2f04201"
STATE_UUID = "6f30d211-2f6d-4a8c-9f78-42d8d2f04201"
WRITE_UUID = "6f30d212-2f6d-4a8c-9f78-42d8d2f04201"
EVENT_UUID = "6f30d213-2f6d-4a8c-9f78-42d8d2f04201"
DEFAULT_NAME = "Codex XTEINK"


def build_payload(args: argparse.Namespace) -> dict:
    if args.json_file:
        return json.loads(Path(args.json_file).read_text(encoding="utf-8"))
    if args.json:
        return json.loads(args.json)

    return {
        "model-with-reasoning": args.model,
        "project-name": args.project,
        "git-branch": args.branch,
        "run-state": args.state,
        "context-used": f"{args.context}% used",
        "context_pct": args.context,
        "used-tokens": f"{args.tokens} total used",
        "five-hour-limit": f"5h {args.five_hour}%",
        "five_hour_pct": args.five_hour,
        "weekly-limit": f"weekly {args.weekly}%",
        "weekly_pct": args.weekly,
        "task-progress": args.tasks,
        "goal_text": args.goal,
    }


async def find_device(name: str, timeout: float):
    try:
        from bleak import BleakScanner
    except ImportError as exc:
        raise SystemExit("缺少 bleak：先运行 `python -m pip install bleak`") from exc

    devices = await BleakScanner.discover(timeout=timeout, return_adv=True)
    for device, adv in devices.values():
        if device.name == name or adv.local_name == name:
            return device
        if SERVICE_UUID.lower() in [uuid.lower() for uuid in adv.service_uuids]:
            return device
    return None


async def send_payload(args: argparse.Namespace) -> int:
    from bleak import BleakClient

    payload = json.dumps(build_payload(args), ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    if len(payload) > 620:
        raise SystemExit(f"payload 太长：{len(payload)} bytes，建议缩短字段文本")

    address = args.address
    if not address:
        device = await find_device(args.name, args.scan_timeout)
        if not device:
            raise SystemExit(f"没有扫描到 BLE 设备 `{args.name}`")
        address = device.address

    events: list[str] = []

    def on_event(_: int, data: bytearray) -> None:
        text = data.decode("utf-8", errors="replace")
        events.append(text)
        print(text)

    async with BleakClient(address, timeout=args.connect_timeout) as client:
        await client.start_notify(EVENT_UUID, on_event)
        await client.write_gatt_char(WRITE_UUID, payload, response=True)
        await asyncio.sleep(args.wait)
        try:
            state = await client.read_gatt_char(STATE_UUID)
            print(state.decode("utf-8", errors="replace"))
        except Exception:
            pass
        await client.stop_notify(EVENT_UUID)

    return 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--name", default=DEFAULT_NAME)
    parser.add_argument("--address")
    parser.add_argument("--scan-timeout", type=float, default=8)
    parser.add_argument("--connect-timeout", type=float, default=12)
    parser.add_argument("--wait", type=float, default=1.2)
    parser.add_argument("--json")
    parser.add_argument("--json-file")
    parser.add_argument("--model", default="gpt-5.5 high fast")
    parser.add_argument("--project", default="MatrixSpec")
    parser.add_argument("--branch", default="main")
    parser.add_argument("--state", default="Ready")
    parser.add_argument("--context", type=int, default=79)
    parser.add_argument("--tokens", default="1.46M")
    parser.add_argument("--five-hour", type=int, default=97)
    parser.add_argument("--weekly", type=int, default=93)
    parser.add_argument("--tasks", default="4/4")
    parser.add_argument("--goal", default="Goal achieved (53m)")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    return asyncio.run(send_payload(args))


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
