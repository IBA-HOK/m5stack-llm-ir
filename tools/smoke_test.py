#!/usr/bin/env python3
"""Smoke test for the m5stack-llm-ir HTTP API.

Only uses the Python standard library, so it runs on any machine that can reach
the device (local LAN, Tailscale, Cloudflare Tunnel or the Workers relay).

Usage:
    python3 tools/smoke_test.py --base-url http://m5ir.local --api-key <API_KEY>
    python3 tools/smoke_test.py --base-url https://xxx.workers.dev --api-key <KEY> \
        --timeout 40

Exit code 0 = all mandatory checks passed. IR checks are skipped automatically
when the firmware runs with the stub IR backend (Unit IR not connected yet).
"""

from __future__ import annotations

import argparse
import json
import sys
import urllib.error
import urllib.request
from typing import Any


class Client:
    def __init__(self, base_url: str, api_key: str, timeout: float) -> None:
        self.base_url = base_url.rstrip("/")
        self.api_key = api_key
        self.timeout = timeout

    def request(
        self,
        method: str,
        path: str,
        body: dict[str, Any] | None = None,
        auth: bool = True,
        timeout: float | None = None,
    ) -> tuple[int, Any]:
        url = f"{self.base_url}{path}"
        data = None
        headers = {"Accept": "application/json"}
        if body is not None:
            data = json.dumps(body).encode("utf-8")
            headers["Content-Type"] = "application/json"
        if auth and self.api_key:
            headers["Authorization"] = f"Bearer {self.api_key}"

        request = urllib.request.Request(url, data=data, headers=headers, method=method)
        try:
            with urllib.request.urlopen(request, timeout=timeout or self.timeout) as response:
                raw = response.read()
                return response.status, _decode(raw)
        except urllib.error.HTTPError as error:
            raw = error.read()
            return error.code, _decode(raw)
        except urllib.error.URLError as error:
            raise SystemExit(f"cannot reach {url}: {error.reason}") from error


def _decode(raw: bytes) -> Any:
    text = raw.decode("utf-8", errors="replace")
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        return text


class Result:
    def __init__(self) -> None:
        self.failures: list[str] = []

    def check(self, name: str, condition: bool, detail: str = "") -> None:
        status = "PASS" if condition else "FAIL"
        print(f"[{status}] {name}" + (f" - {detail}" if detail else ""))
        if not condition:
            self.failures.append(name)


def error_code(payload: Any) -> str:
    if isinstance(payload, dict):
        error = payload.get("error")
        if isinstance(error, dict):
            return str(error.get("code", ""))
    return ""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-url", default="http://m5ir.local", help="API base URL")
    parser.add_argument("--api-key", default="", help="Bearer API key (M5IR_API_KEY)")
    parser.add_argument("--timeout", type=float, default=40.0,
                        help="HTTP timeout in seconds (must exceed a learn request)")
    parser.add_argument("--skip-ir", action="store_true", help="skip IR learn/test checks")
    args = parser.parse_args()

    client = Client(args.base_url, args.api_key, args.timeout)
    result = Result()

    # --- health -----------------------------------------------------------
    status, health = client.request("GET", "/api/health", auth=False)
    result.check("GET /api/health returns 200", status == 200, f"status={status}")
    result.check("health.ok is true", isinstance(health, dict) and health.get("ok") is True)
    if isinstance(health, dict):
        state = health.get("state")
        wifi = health.get("wifi") or {}
        ir = health.get("ir") or {}
        relay = health.get("relay") or {}
        print(f"       version={health.get('version')} state={state} "
              f"wifi={wifi.get('connected')} ip={wifi.get('ip')}")
        print(f"       ir.hardware={ir.get('hardware')} relay={relay.get('enabled')}/{relay.get('connected')} "
              f"devices={health.get('devices')}")
        result.check("state is a known value",
                     state in {"boot", "connecting", "idle", "learning", "sending", "error"},
                     f"state={state}")

    # --- auth -------------------------------------------------------------
    if args.api_key:
        status, payload = client.request("GET", "/api/devices", auth=False)
        result.check("GET /api/devices without token -> 401", status == 401, f"status={status}")
        result.check("401 body uses the error envelope", error_code(payload) == "UNAUTHORIZED",
                     f"code={error_code(payload)}")

    status, devices = client.request("GET", "/api/devices")
    result.check("GET /api/devices returns 200", status == 200, f"status={status}")
    result.check("devices response has ok=true", isinstance(devices, dict) and devices.get("ok") is True)

    status, payload = client.request("GET", "/api/nope")
    result.check("unknown endpoint -> 404", status == 404, f"status={status}")

    # --- IR (optional depending on hardware availability) ------------------
    hardware = bool(health.get("ir", {}).get("hardware")) if isinstance(health, dict) else False
    if args.skip_ir:
        print("[SKIP] IR checks disabled with --skip-ir")
    elif not hardware:
        print("[SKIP] Unit IR is not connected (ir.hardware=false); IR learn/test skipped")
    else:
        timeout_ms = 5000
        status, learn = client.request("POST", "/api/ir/learn", {"timeout_ms": timeout_ms},
                                       timeout=timeout_ms / 1000 + 15)
        capture_id = learn.get("capture_id") if isinstance(learn, dict) else None
        if status == 408:
            print("[INFO] no remote control was pressed during the learn window (408 is expected)")
        else:
            result.check("POST /api/ir/learn returns 200", status == 200, f"status={status}")
            result.check("learn returns a capture_id", isinstance(capture_id, str) and capture_id,
                         f"capture_id={capture_id}")

        if isinstance(capture_id, str) and capture_id:
            status, tested = client.request("POST", "/api/ir/test", {"capture_id": capture_id})
            result.check("POST /api/ir/test returns 200", status == 200, f"status={status}")

            device_id = "smoke_test_device"
            status, saved = client.request(
                "POST", f"/api/devices/{device_id}/actions",
                {"label": "smoke test", "action": "power", "capture_id": capture_id},
            )
            result.check("save action returns 201", status == 201, f"status={status}")
            result.check("save reports stored_format",
                         isinstance(saved, dict) and saved.get("stored_format") in {"protocol", "raw"})

            status, sent = client.request("POST", "/api/ir/send",
                                          {"device": device_id, "action": "power"})
            result.check("POST /api/ir/send returns 200", status == 200, f"status={status}")
            result.check("send reports executed_steps",
                         isinstance(sent, dict) and sent.get("executed_steps") == 1)

            status, _ = client.request("DELETE", f"/api/devices/{device_id}")
            result.check("cleanup DELETE device returns 200", status == 200, f"status={status}")

    print()
    if result.failures:
        print(f"FAILED: {len(result.failures)} check(s): {', '.join(result.failures)}")
        return 1
    print("ALL CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
