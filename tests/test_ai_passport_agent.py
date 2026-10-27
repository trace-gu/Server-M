#!/usr/bin/env python3
"""Host tests for the server-side relay agent (signing, parsing, auth)."""

import importlib.util
import json
import threading
import unittest
import urllib.error
import urllib.request
from pathlib import Path
from types import SimpleNamespace

MODULE_PATH = Path(__file__).resolve().parents[1] / "server" / "ai_passport_agent.py"

spec = importlib.util.spec_from_file_location("ai_passport_agent", MODULE_PATH)
agent_mod = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(agent_mod)


class SigningTest(unittest.TestCase):
    def test_1panel_token_matches_known_vector(self) -> None:
        # md5("1panel" + "key-abc" + "1700000000")
        self.assertEqual(
            agent_mod.token_1panel("key-abc", "1700000000"),
            "82dfcebc93e674af46f798cfed905f18",
        )

    def test_baota_token_matches_known_vector(self) -> None:
        # md5("1700000000" + md5("key-abc"))
        self.assertEqual(
            agent_mod.sign_baota("key-abc", "1700000000"),
            "eaff3615c3dadfc43de04cb8c43e092a",
        )


class Parse1PanelTest(unittest.TestCase):
    def test_dashboard_current_payload(self) -> None:
        payload = {
            "code": 200,
            "data": {
                "cpuUsedPercent": 12.5,
                "memoryUsedPercent": 63.2,
                "swapMemoryUsedPercent": 0,
                "load1": 0.42,
                "uptime": 86400,
                "diskData": [
                    {"path": "/", "usedPercent": 31},
                    {"path": "/data", "usedPercent": 87.6},
                ],
            },
        }
        out = agent_mod.parse_1panel(payload)
        self.assertEqual(out["panel"], "1Panel")
        self.assertEqual(out["cpu"], 12)
        self.assertEqual(out["mem"], 63)
        self.assertEqual(out["swap"], 0)
        self.assertEqual(out["disk"], 88)
        self.assertEqual(out["load1"], 0.42)
        self.assertEqual(out["uptime"], 86400)

    def test_current_info_nesting_and_missing_fields(self) -> None:
        out = agent_mod.parse_1panel({"data": {"currentInfo": {"cpuUsedPercent": 5}}})
        self.assertEqual(out["cpu"], 5)
        self.assertEqual(out["mem"], -1)
        self.assertEqual(out["disk"], -1)

    def test_garbage_does_not_raise(self) -> None:
        out = agent_mod.parse_1panel({"data": "unexpected"})
        self.assertEqual(out["panel"], "1Panel")


class ParseBaotaTest(unittest.TestCase):
    def test_system_disk_network(self) -> None:
        total = {
            "memTotal": 8 * 1024 * 1024,
            "memRealUsed": 2 * 1024 * 1024,
            "cpuRealUsed": 3.5,
            "version": "9.2.0",
        }
        disk = [
            {"path": "/", "size": ["50", "10", "40", "20%"], "byte_size": [50, 10, 40, 20]},
            {"path": "/data", "size": ["100", "91", "9", "91%"], "byte_size": [100, 91, 9, 91]},
        ]
        network = {"down": 2048, "up": 1024, "load": {"one": 1.25}}
        out = agent_mod.parse_baota(total, disk, network, {})
        self.assertEqual(out["panel"], "宝塔")
        self.assertEqual(out["cpu"], 4)
        self.assertEqual(out["mem"], 25)
        self.assertEqual(out["disk"], 91)
        self.assertEqual(out["net_rx"], 2048 * 1024)
        self.assertEqual(out["net_tx"], 1024 * 1024)
        self.assertEqual(out["load1"], 1.25)
        self.assertEqual(out["panel_version"], "9.2.0")

    def test_missing_disk_and_network(self) -> None:
        out = agent_mod.parse_baota({"memTotal": 0, "memRealUsed": 1}, None, None, {})
        self.assertEqual(out["mem"], -1)
        self.assertEqual(out["disk"], -1)
        self.assertEqual(out["cpu"], -1)
        self.assertNotIn("net_rx", out)


class ConfigTest(unittest.TestCase):
    def test_roundtrip_and_truncated_input(self) -> None:
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "agent.json"
            cfg = agent_mod.default_config()
            cfg.update({"token": "abc", "port": 8899, "panel": "baota"})
            agent_mod.save_config(cfg, path)
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            loaded = agent_mod.load_config(path)
            self.assertEqual(loaded["token"], "abc")
            self.assertEqual(loaded["panel"], "baota")

            path.write_text(json.dumps({"port": 0, "trend_step": 1}), encoding="utf-8")
            loaded = agent_mod.load_config(path)
            self.assertEqual(loaded["port"], agent_mod.DEFAULT_PORT)
            self.assertGreaterEqual(loaded["trend_step"], 60)

    def test_missing_file_returns_defaults(self) -> None:
        loaded = agent_mod.load_config(Path("/nonexistent/ai-passport/agent.json"))
        self.assertEqual(loaded["bind"], agent_mod.DEFAULT_BIND)
        self.assertEqual(loaded["panel"], "auto")


class StatusHTTPTest(unittest.TestCase):
    def setUp(self) -> None:
        self.agent = SimpleNamespace(
            cfg={"token": "secret-token"},
            verbose=False,
            status=lambda: {"online": True, "cpu": 7, "trend": {"step": 300, "cpu": [], "mem": []}},
        )
        self.server = agent_mod.AgentHTTPServer(
            ("127.0.0.1", 0), agent_mod.StatusRequestHandler, self.agent
        )
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.base = f"http://127.0.0.1:{self.server.server_address[1]}"

    def tearDown(self) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)

    def _get(self, path: str, token: str | None = None):
        req = urllib.request.Request(self.base + path)
        if token is not None:
            req.add_header("X-Api-Token", token)
        with urllib.request.urlopen(req, timeout=3) as resp:
            return resp.status, json.loads(resp.read().decode("utf-8"))

    def test_healthz_is_public(self) -> None:
        status, body = self._get("/healthz")
        self.assertEqual(status, 200)
        self.assertTrue(body["ok"])

    def test_status_requires_token(self) -> None:
        with self.assertRaises(urllib.error.HTTPError) as ctx:
            self._get("/status")
        self.assertEqual(ctx.exception.code, 401)
        ctx.exception.close()

        with self.assertRaises(urllib.error.HTTPError) as ctx:
            self._get("/status", "wrong")
        self.assertEqual(ctx.exception.code, 401)
        ctx.exception.close()

    def test_status_with_token(self) -> None:
        status, body = self._get("/status", "secret-token")
        self.assertEqual(status, 200)
        self.assertTrue(body["online"])
        self.assertEqual(body["cpu"], 7)

    def test_unknown_path_is_404(self) -> None:
        with self.assertRaises(urllib.error.HTTPError) as ctx:
            self._get("/nope")
        self.assertEqual(ctx.exception.code, 404)
        ctx.exception.close()


if __name__ == "__main__":
    unittest.main()
