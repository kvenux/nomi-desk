#!/usr/bin/env python3
"""Codex status bridge.

Collects Codex lifecycle events from a local hook HTTP endpoint, enriches them
with Codex app-server config/rate-limit/token data, and writes compact JSON to
the ESP32 BLE status display.
"""

from __future__ import annotations

import argparse
import asyncio
import ctypes
from contextlib import AsyncExitStack
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import threading
import time
import urllib.request
from dataclasses import dataclass, field
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any

from bleak import BleakClient, BleakScanner
import serial
import websockets


DEVICE_NAMES = ("Codex Status", "Codex RLCD", "Codex XTEINK")
PREFERRED_ADDRESSES = tuple(
    part.strip().upper()
    for part in os.environ.get("CODEX_STATUS_BLE_ADDRESS", "A4:CB:8F:D7:6A:3D").split(",")
    if part.strip()
)
SERVICE_UUID = "4c41555a-4465-7669-6365-000000000001"
RX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000002"
TX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000003"

APP_SERVER_URL = "ws://127.0.0.1:49300"
APP_READYZ = "http://127.0.0.1:49300/readyz"
HOOK_HOST = "127.0.0.1"
HOOK_PORT = 8797


@dataclass
class BridgeState:
    event: str = "boot"
    codex_state: str = "offline"
    cwd: str = ""
    session_name: str = "current session"
    prompt: str = "waiting for prompt"
    model: str = "unknown"
    effort: str = ""
    tier: str = ""
    context_pct: int = 0
    used_tokens_k: int = 0
    five_left: int = -1
    weekly_left: int = -1
    last_hook_at: float = field(default_factory=time.time)


state = BridgeState()
state_lock = threading.Lock()
push_queue: asyncio.Queue[None] | None = None
loop_ref: asyncio.AbstractEventLoop | None = None
session_path: Path | None = None


def log(message: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {message}", flush=True)


KEYEVENTF_KEYUP = 0x0002
VK_CONTROL = 0x11
VK_U = 0x55
VK_RETURN = 0x0D
VK_CAPITAL = 0x14


def key_event(vk: int, key_up: bool = False) -> None:
    flags = KEYEVENTF_KEYUP if key_up else 0
    ctypes.windll.user32.keybd_event(vk, 0, flags, 0)


def tap_key(vk: int) -> None:
    key_event(vk, False)
    time.sleep(0.035)
    key_event(vk, True)


def handle_key_event(payload: bytes) -> None:
    try:
        data = json.loads(payload.decode("utf-8", errors="replace"))
    except json.JSONDecodeError:
        return
    key = data.get("key")
    action = data.get("action")
    if not key:
        return
    log(f"BLE key {key}:{action}")
    if key == "ctrl_u" and action == "tap":
        key_event(VK_CONTROL, False)
        tap_key(VK_U)
        key_event(VK_CONTROL, True)
    elif key == "enter" and action == "tap":
        tap_key(VK_RETURN)
    elif key == "capslock" and action == "down":
        key_event(VK_CAPITAL, False)
    elif key == "capslock" and action == "up":
        key_event(VK_CAPITAL, True)


def event_to_state(event: str) -> str:
    mapping = {
        "SessionStart": "idle",
        "UserPromptSubmit": "active",
        "PermissionRequest": "waiting_approval",
        "PreToolUse": "active",
        "PostToolUse": "active",
        "Stop": "idle",
    }
    return mapping.get(event, "active")


def build_payload() -> dict[str, Any]:
    with state_lock:
        return {
            "state": state.codex_state,
            "model": state.model,
            "session": state.session_name,
            "prompt": state.prompt,
            "effort": state.effort,
            "tier": state.tier,
            "event": state.event,
            "context_pct": state.context_pct,
            "used_tokens_k": state.used_tokens_k,
            "five_left": state.five_left,
            "weekly_left": state.weekly_left,
            "time": time.strftime("%H:%M"),
        }


def encode_payload() -> bytes:
    return json.dumps(build_payload(), ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def trim_utf8(text: str, max_bytes: int) -> str:
    data = text.encode("utf-8")
    if len(data) <= max_bytes:
        return text
    return data[:max_bytes].decode("utf-8", errors="ignore")


def find_prompt_value(value: Any) -> str:
    if isinstance(value, str):
        return value
    if isinstance(value, dict):
        for key in ("prompt", "user_prompt", "input", "text", "message", "content"):
            found = find_prompt_value(value.get(key))
            if found:
                return found
        for child in value.values():
            found = find_prompt_value(child)
            if found:
                return found
    if isinstance(value, list):
        for child in value:
            found = find_prompt_value(child)
            if found:
                return found
    return ""


def extract_prompt_from_hook(data: dict[str, Any]) -> str:
    stdin = str(data.get("stdin") or "").strip()
    if not stdin:
        return ""
    try:
        parsed = json.loads(stdin)
        prompt = find_prompt_value(parsed)
    except json.JSONDecodeError:
        prompt = stdin
    prompt = " ".join(prompt.split())
    return trim_utf8(prompt, 96)


def update_from_token_count(payload: dict[str, Any]) -> bool:
    info = payload.get("info") or {}
    usage = info.get("last_token_usage") or info.get("total_token_usage") or {}
    total_tokens = int(usage.get("total_tokens") or 0)
    context_window = int(info.get("model_context_window") or 0)

    rate = payload.get("rate_limits") or {}
    primary = rate.get("primary") or {}
    secondary = rate.get("secondary") or {}

    changed = False
    with state_lock:
        if total_tokens > 0:
            state.used_tokens_k = int(round(total_tokens / 1000))
            state.context_pct = int(round(total_tokens * 100 / context_window)) if context_window > 0 else 0
            changed = True
        if "used_percent" in primary:
            state.five_left = max(0, min(100, 100 - int(float(primary["used_percent"]))))
            changed = True
        if "used_percent" in secondary:
            state.weekly_left = max(0, min(100, 100 - int(float(secondary["used_percent"]))))
            changed = True

    return changed


def refresh_token_count_from_session_file() -> bool:
    if not session_path or not session_path.exists():
        return False

    last_token_count: dict[str, Any] | None = None
    try:
        with session_path.open("r", encoding="utf-8", errors="replace") as handle:
            for line in handle:
                try:
                    row = json.loads(line)
                except json.JSONDecodeError:
                    continue
                payload = row.get("payload") or {}
                if row.get("type") == "event_msg" and payload.get("type") == "token_count":
                    last_token_count = payload
    except OSError as exc:
        log(f"token_count read failed: {exc}")
        return False

    if not last_token_count:
        return False

    changed = update_from_token_count(last_token_count)
    if changed:
        with state_lock:
            log(
                "token_count snapshot: "
                f"context={state.context_pct}% used={state.used_tokens_k}K "
                f"5h={state.five_left} weekly={state.weekly_left}"
            )
    return changed


def request_push() -> None:
    if loop_ref is None or push_queue is None:
        return
    loop_ref.call_soon_threadsafe(push_queue.put_nowait, None)


class HookHandler(BaseHTTPRequestHandler):
    def do_POST(self) -> None:  # noqa: N802
        if self.path != "/hook":
            self.send_response(404)
            self.end_headers()
            return

        length = int(self.headers.get("content-length", "0"))
        raw = self.rfile.read(length)
        try:
            data = json.loads(raw.decode("utf-8", errors="replace"))
        except json.JSONDecodeError:
            data = {}

        event = str(data.get("event") or "Unknown")
        prompt = extract_prompt_from_hook(data) if event == "UserPromptSubmit" else ""
        with state_lock:
            state.event = event
            state.codex_state = event_to_state(event)
            state.cwd = str(data.get("cwd") or state.cwd)
            if prompt:
                state.prompt = prompt
            state.last_hook_at = time.time()

        log(f"hook: {event}" + (f" prompt={prompt}" if prompt else ""))
        refresh_token_count_from_session_file()
        request_push()

        self.send_response(204)
        self.end_headers()

    def log_message(self, fmt: str, *args: Any) -> None:
        return


def start_hook_server() -> ThreadingHTTPServer:
    server = ThreadingHTTPServer((HOOK_HOST, HOOK_PORT), HookHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    log(f"hook server listening on http://{HOOK_HOST}:{HOOK_PORT}/hook")
    return server


def app_server_ready() -> bool:
    try:
        with urllib.request.urlopen(APP_READYZ, timeout=1.0) as resp:
            return resp.status == 200
    except Exception:
        return False


def resolve_codex_bin(override: str | None = None) -> str:
    if override:
        return override

    candidates = [
        "codex",
        "codex.cmd",
        "codex.exe",
        r"C:\nvm4w\nodejs\codex.cmd",
        os.path.expandvars(r"%APPDATA%\npm\codex.cmd"),
        os.path.expandvars(r"%LOCALAPPDATA%\Programs\nodejs\codex.cmd"),
    ]
    for candidate in candidates:
        found = shutil.which(candidate)
        if found:
            return found
        if os.path.isfile(candidate):
            return candidate
    return "codex"


def ensure_app_server(codex_bin: str | None = None) -> subprocess.Popen[str] | None:
    if app_server_ready():
        return None
    log("starting codex app-server on ws://127.0.0.1:49300")
    executable = resolve_codex_bin(codex_bin)
    proc = subprocess.Popen(
        [executable, "app-server", "--listen", APP_SERVER_URL],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    for _ in range(30):
        if app_server_ready():
            return proc
        time.sleep(0.2)
    log("warning: app-server did not become ready")
    return proc


class CodexRpc:
    def __init__(self) -> None:
        self.next_id = 1
        self.pending: dict[int, asyncio.Future[dict[str, Any]]] = {}
        self.ws: Any = None

    async def request(self, method: str, params: Any = None) -> dict[str, Any]:
        req_id = self.next_id
        self.next_id += 1
        fut: asyncio.Future[dict[str, Any]] = asyncio.get_running_loop().create_future()
        self.pending[req_id] = fut
        msg: dict[str, Any] = {"jsonrpc": "2.0", "id": req_id, "method": method}
        if params is not None:
            msg["params"] = params
        await self.ws.send(json.dumps(msg))
        try:
            return await asyncio.wait_for(fut, timeout=10)
        finally:
            self.pending.pop(req_id, None)

    async def initialized(self) -> None:
        await self.ws.send(json.dumps({"jsonrpc": "2.0", "method": "initialized"}))


async def refresh_codex_snapshot(rpc: CodexRpc) -> None:
    global session_path
    try:
        cfg_res = await rpc.request("config/read", {"cwd": state.cwd or None, "includeLayers": False})
        cfg = cfg_res.get("result", {}).get("config", {})
        rate_res = await rpc.request("account/rateLimits/read")
        rate = rate_res.get("result", {}).get("rateLimits", {})
        threads_res = await rpc.request("thread/list", {"cwd": state.cwd or None})
    except Exception as exc:
        log(f"codex snapshot failed: {exc}")
        return

    primary = rate.get("primary") or {}
    secondary = rate.get("secondary") or {}
    with state_lock:
        state.model = str(cfg.get("model") or state.model)
        state.effort = str(cfg.get("model_reasoning_effort") or state.effort)
        state.tier = str(cfg.get("service_tier") or state.tier)
        if state.codex_state == "offline":
            state.codex_state = "idle"
        if "usedPercent" in primary:
            state.five_left = max(0, min(100, 100 - int(primary["usedPercent"])))
        if "usedPercent" in secondary:
            state.weekly_left = max(0, min(100, 100 - int(secondary["usedPercent"])))
        log(
            "codex snapshot: "
            f"{state.model} {state.effort} {state.tier} "
            f"5h={state.five_left} weekly={state.weekly_left}"
        )

    threads = threads_res.get("result", {}).get("data", [])
    if threads:
        newest = max(threads, key=lambda item: int(item.get("updatedAt") or item.get("createdAt") or 0))
        label = str(newest.get("name") or newest.get("preview") or newest.get("id") or "current session")
        label = " ".join(label.split())
        path = newest.get("path")
        with state_lock:
            state.session_name = trim_utf8(label, 24)
        if path:
            session_path = Path(path)
            refresh_token_count_from_session_file()


def update_from_notification(msg: dict[str, Any]) -> bool:
    method = msg.get("method")
    params = msg.get("params") or {}
    changed = False

    with state_lock:
        if method == "thread/status/changed":
            st = params.get("status") or {}
            typ = st.get("type")
            flags = st.get("activeFlags") or []
            if typ == "idle":
                state.codex_state = "idle"
            elif typ == "active" and "waitingOnApproval" in flags:
                state.codex_state = "waiting_approval"
            elif typ == "active" and "waitingOnUserInput" in flags:
                state.codex_state = "waiting_input"
            elif typ == "active":
                state.codex_state = "active"
            elif typ == "systemError":
                state.codex_state = "failed"
            changed = True
        elif method == "turn/started":
            state.codex_state = "active"
            state.event = "TurnStarted"
            changed = True
        elif method == "turn/completed":
            turn = params.get("turn") or {}
            state.codex_state = "failed" if turn.get("status") == "failed" else "idle"
            state.event = "TurnCompleted"
            changed = True
        elif method == "thread/tokenUsage/updated":
            usage = params.get("tokenUsage") or {}
            total = usage.get("total") or {}
            tokens = int(total.get("totalTokens") or 0)
            window = int(usage.get("modelContextWindow") or 0)
            state.used_tokens_k = int(round(tokens / 1000))
            state.context_pct = int(round(tokens * 100 / window)) if window > 0 else 0
            changed = True
        elif method == "experimental/event" and params.get("type") == "token_count":
            changed = update_from_token_count(params)
        elif method == "account/rateLimits/updated":
            rate = params.get("rateLimits") or {}
            primary = rate.get("primary") or {}
            secondary = rate.get("secondary") or {}
            if "usedPercent" in primary:
                state.five_left = max(0, min(100, 100 - int(primary["usedPercent"])))
            if "usedPercent" in secondary:
                state.weekly_left = max(0, min(100, 100 - int(secondary["usedPercent"])))
            changed = True

    return changed


async def codex_task() -> None:
    while True:
        try:
            rpc = CodexRpc()
            async with websockets.connect(APP_SERVER_URL) as ws:
                rpc.ws = ws

                async def reader() -> None:
                    async for raw in ws:
                        msg = json.loads(raw)
                        if "id" in msg and msg["id"] in rpc.pending:
                            fut = rpc.pending.pop(msg["id"])
                            if not fut.done():
                                fut.set_result(msg)
                        elif update_from_notification(msg):
                            request_push()

                reader_task = asyncio.create_task(reader())
                init = await rpc.request(
                    "initialize",
                    {
                        "clientInfo": {"name": "codex-status-bridge", "version": "0.1.0"},
                        "capabilities": {"experimentalApi": True},
                    },
                )
                if "error" in init:
                    log(f"initialize error: {init['error']}")
                await rpc.initialized()
                await refresh_codex_snapshot(rpc)
                request_push()

                async def periodic() -> None:
                    while True:
                        await asyncio.sleep(60)
                        await refresh_codex_snapshot(rpc)
                        request_push()

                periodic_task = asyncio.create_task(periodic())
                try:
                    await reader_task
                finally:
                    periodic_task.cancel()
                    reader_task.cancel()
        except Exception as exc:
            log(f"app-server disconnected: {exc}")
            with state_lock:
                state.codex_state = "offline"
            request_push()
            await asyncio.sleep(3)


def parse_addresses(address: str | None) -> list[str]:
    if address:
        return [part.strip() for part in address.split(",") if part.strip()]
    return []


async def find_devices(address: str | None, allow_service_fallback: bool = False) -> list[str]:
    requested = parse_addresses(address)
    if requested:
        return requested
    log(f"scanning for BLE device names: {', '.join(DEVICE_NAMES)}")
    devices = await BleakScanner.discover(timeout=8.0, return_adv=True)
    service_uuid = SERVICE_UUID.lower()
    candidates: list[tuple[int, str, str]] = []
    fallback_candidates: list[tuple[int, str, str]] = []
    for dev, adv in devices.values():
        adv_name = adv.local_name or dev.name
        service_uuids = [uuid.lower() for uuid in adv.service_uuids]
        if adv_name in DEVICE_NAMES:
            label = adv_name
            priority = DEVICE_NAMES.index(label)
            candidates.append((priority, label, dev.address))
        elif dev.address.upper() in PREFERRED_ADDRESSES:
            candidates.append((len(DEVICE_NAMES), "preferred address", dev.address))
        elif allow_service_fallback and service_uuid in service_uuids:
            fallback_candidates.append((len(DEVICE_NAMES), SERVICE_UUID, dev.address))
    if not candidates and allow_service_fallback:
        candidates = fallback_candidates
    if candidates:
        seen: set[str] = set()
        targets: list[str] = []
        for _priority, label, addr in sorted(candidates, key=lambda item: item[0]):
            if addr in seen:
                continue
            seen.add(addr)
            targets.append(addr)
            log(f"found {label}: {addr}")
        return targets
    return []


async def ble_task(address: str | None, allow_service_fallback: bool) -> None:
    while True:
        targets = await find_devices(address, allow_service_fallback)
        if not targets:
            log("device not found; retrying")
            await asyncio.sleep(5)
            continue

        try:
            async with AsyncExitStack() as stack:
                clients: list[tuple[str, BleakClient]] = []
                for target in targets:
                    log(f"connecting BLE {target}")
                    client = await stack.enter_async_context(BleakClient(target))
                    clients.append((target, client))
                    log(f"BLE connected {target}")
                    await client.start_notify(TX_CHAR_UUID, lambda _sender, data: handle_key_event(data))

                payload = encode_payload()
                log(f"BLE write {payload.decode('utf-8', errors='replace')}")
                for target, client in clients:
                    await client.write_gatt_char(RX_CHAR_UUID, payload, response=True)
                    log(f"BLE wrote {target}")
                while True:
                    await push_queue.get()  # type: ignore[union-attr]
                    payload = encode_payload()
                    log(f"BLE write {payload.decode('utf-8', errors='replace')}")
                    for target, client in clients:
                        await client.write_gatt_char(RX_CHAR_UUID, payload, response=True)
                        log(f"BLE wrote {target}")
        except Exception as exc:
            log(f"BLE disconnected/error: {exc}")
            await asyncio.sleep(3)


async def serial_task(port: str, baud: int) -> None:
    while True:
        try:
            log(f"opening serial {port} @ {baud}")
            with serial.Serial(port, baud, timeout=0.2, write_timeout=2.0) as ser:
                await asyncio.sleep(1.0)
                payload = encode_payload() + b"\n"
                log(f"SERIAL write {payload.decode('utf-8', errors='replace').strip()}")
                ser.write(payload)
                ser.flush()
                while True:
                    await push_queue.get()  # type: ignore[union-attr]
                    payload = encode_payload() + b"\n"
                    log(f"SERIAL write {payload.decode('utf-8', errors='replace').strip()}")
                    ser.write(payload)
                    ser.flush()
        except Exception as exc:
            log(f"serial disconnected/error: {exc}")
            await asyncio.sleep(3)


async def main() -> None:
    global push_queue, loop_ref
    parser = argparse.ArgumentParser()
    parser.add_argument("--address", help="BLE address/UUID if scanning by name is not enough")
    parser.add_argument("--allow-service-fallback", action="store_true", help="Allow connecting to any device advertising the shared Codex service UUID")
    parser.add_argument("--serial", help="Serial port for USB-connected display, for example COM5")
    parser.add_argument("--serial-baud", type=int, default=115200)
    parser.add_argument("--no-ble", action="store_true", help="Disable BLE status forwarding")
    parser.add_argument("--codex-bin", help="Path to codex/codex.cmd for starting app-server")
    parser.add_argument("--no-app-server-start", action="store_true")
    args = parser.parse_args()

    loop_ref = asyncio.get_running_loop()
    push_queue = asyncio.Queue()
    start_hook_server()
    proc = None if args.no_app_server_start else ensure_app_server(args.codex_bin)

    try:
        tasks = [codex_task()]
        if not args.no_ble:
            tasks.append(ble_task(args.address, args.allow_service_fallback))
        if args.serial:
            tasks.append(serial_task(args.serial, args.serial_baud))
        await asyncio.gather(*tasks)
    finally:
        if proc:
            proc.terminate()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
