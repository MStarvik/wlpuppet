#!/usr/bin/env python3
"""End-to-end test: wlpuppet-mcp in front of a headless sway, driving a GTK app.

Runs everything in a private XDG_RUNTIME_DIR, so it never touches the user's
session. Exits with 77 (skipped) if sway or GTK 4 for Python is missing.
"""

import argparse
import base64
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib

SKIP = 77


class McpClient:
    def __init__(self, argv, env):
        self.proc = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=env)
        self.next_id = 1

    def request(self, method, params=None):
        rid = self.next_id
        self.next_id += 1
        message = {"jsonrpc": "2.0", "id": rid, "method": method,
                   "params": dict(params or {}, _meta={"io.modelcontextprotocol/protocolVersion": "2026-07-28"})}
        self.proc.stdin.write((json.dumps(message) + "\n").encode())
        self.proc.stdin.flush()
        while True:
            line = self.proc.stdout.readline()
            if not line:
                raise RuntimeError("wlpuppet-mcp exited")
            response = json.loads(line)
            if response.get("id") == rid:
                return response

    def call(self, name, **arguments):
        response = self.request("tools/call", {"name": name, "arguments": arguments})
        assert "result" in response, response
        result = response["result"]
        texts = [c["text"] for c in result["content"] if c["type"] == "text"]
        images = [base64.b64decode(c["data"]) for c in result["content"] if c["type"] == "image"]
        print(f"[{name}] {' | '.join(texts)}", flush=True)
        assert not result.get("isError"), f"{name} failed: {texts}"
        return result, texts, images

    def close(self):
        self.proc.stdin.close()
        self.proc.wait(timeout=10)


def png_size(data):
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    return struct.unpack(">II", data[16:24])


def png_pixel(data, x, y):
    width, height = png_size(data)
    pos, idat = 8, b""
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        if kind == b"IDAT":
            idat += data[pos + 8:pos + 8 + length]
        pos += 12 + length
    raw, stride, prev = zlib.decompress(idat), width * 4, bytearray(width * 4)
    for row in range(y + 1):
        kind = raw[row * (stride + 1)]
        line = bytearray(raw[row * (stride + 1) + 1:(row + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - 4] if i >= 4 else 0
            b, c = prev[i], (prev[i - 4] if i >= 4 else 0)
            if kind == 1:
                line[i] = (line[i] + a) & 255
            elif kind == 2:
                line[i] = (line[i] + b) & 255
            elif kind == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
    return tuple(prev[x * 4:x * 4 + 4])


def wait_for(predicate, timeout=10.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(0.1)
    raise TimeoutError


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--apps", required=True)
    args = parser.parse_args()

    if shutil.which("sway") is None:
        print("sway not found; skipping")
        return SKIP
    if subprocess.run([sys.executable, "-c", "import gi; gi.require_version('Gtk', '4.0'); from gi.repository import Gtk"],
                      capture_output=True).returncode != 0:
        print("GTK 4 for Python not available; skipping")
        return SKIP

    with tempfile.TemporaryDirectory(prefix="wlpuppet-test-") as runtime:
        os.chmod(runtime, 0o700)
        display_file = os.path.join(runtime, "display")
        config = os.path.join(runtime, "sway.conf")
        with open(config, "w") as f:
            f.write("output HEADLESS-1 resolution 1024x768\n")
            f.write(f"exec sh -c 'echo $WAYLAND_DISPLAY > {display_file}'\n")
        base_env = {k: v for k, v in os.environ.items() if k not in ("WAYLAND_DISPLAY", "DISPLAY", "SWAYSOCK")}
        base_env["XDG_RUNTIME_DIR"] = runtime
        sway = subprocess.Popen(["sway", "-c", config], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                env=dict(base_env, WLR_BACKENDS="headless", WLR_RENDERER="pixman",
                                         WLR_LIBINPUT_NO_DEVICES="1"))
        client = None
        try:
            display = wait_for(lambda: os.path.exists(display_file) and open(display_file).read().strip())
            env = dict(base_env, WAYLAND_DISPLAY=display, GSETTINGS_BACKEND="memory")
            client = McpClient([args.server], env)

            tools = {t["name"] for t in client.request("tools/list")["result"]["tools"]}
            assert {"screenshot", "click", "type_text", "key", "display_info", "wait_for_window"} <= tools, tools

            # As an agent would: ask how to connect, start the app itself, wait for it.
            result, _, _ = client.call("display_info")
            info = result["structuredContent"]
            app_env = {k: v for k, v in env.items() if k not in info["unset"]}
            app_env.update(info["environment"])
            log_path = os.path.join(runtime, "app.log")
            with open(log_path, "w") as log:
                app = subprocess.Popen([sys.executable, os.path.join(args.apps, "gtk_probe.py")],
                                       stdout=log, stderr=subprocess.STDOUT, env=app_env)
            result, _, _ = client.call("wait_for_window", app_id="org.wlpuppet.Probe")
            assert result["structuredContent"]["pid"] == app.pid

            def output():
                with open(log_path) as f:
                    return f.read()

            layout = {}
            for line in wait_for(lambda: "layout entry" in output() and output()).splitlines():
                if line.startswith("layout "):
                    _, name, x, y, w, h = line.split()
                    layout[name] = (int(x) + int(w) // 2, int(y) + int(h) // 2)

            _, texts, images = client.call("screenshot")
            width, height = png_size(images[0])
            assert width > 400 and height > 300, (width, height)

            client.call("click", x=layout["button"][0], y=layout["button"][1])
            client.call("click", x=layout["button"][0], y=layout["button"][1], count=2)
            wait_for(lambda: "clicked 3" in output())

            # A menu: open it, see it in the screenshot, activate an item with the keyboard.
            _, _, closed = client.call("screenshot")
            _, _, opened = client.call("click", x=layout["menu"][0], y=layout["menu"][1], screenshot=True)
            assert opened[0] != closed[0], "the popup should change the screenshot"
            # GTK focuses the first item when the popup gains keyboard focus.
            client.call("key", keys="Down Return")
            wait_for(lambda: "menu " in output())

            client.call("click", x=layout["entry"][0], y=layout["entry"][1])
            client.call("type_text", text="Grüße ✓")
            wait_for(lambda: "text 'Grüße ✓'" in output())
            client.call("key", keys="ctrl+a BackSpace")
            wait_for(lambda: output().rstrip().endswith("text ''"))

            client.call("scroll", x=layout["list"][0], y=layout["list"][1], dy=3)
            wait_for(lambda: "scrolled" in output())

            _, texts, _ = client.call("close_window")
            assert texts[0].startswith("Closed"), texts
            assert client.call("list_windows")[0]["structuredContent"]["windows"] == []
            app.wait(timeout=10)

            # An application that asks before closing: the dialog is reported and shown.
            stubborn = subprocess.Popen([sys.executable, os.path.join(args.apps, "gtk_probe.py"), "--confirm-close"],
                                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=app_env)
            window = client.call("wait_for_window", app_id="org.wlpuppet.Probe")[0]["structuredContent"]["window"]
            _, texts, images = client.call("close_window", window=window, timeout_ms=1000)
            assert "still open" in texts[0] and "Really quit?" in texts[0], texts
            assert len(images) == 1
            dialog = next(w for w in client.call("list_windows")[0]["structuredContent"]["windows"]
                          if w["title"] == "Really quit?")
            width, height = png_size(images[0])
            # The Quit button fills the dialog below its header bar.
            client.call("click", window=dialog["id"], x=width // 2, y=height * 2 // 3)
            stubborn.wait(timeout=10)
            print("PASS")
            return 0
        finally:
            if client is not None:
                client.close()
            sway.terminate()
            sway.wait(timeout=10)


if __name__ == "__main__":
    sys.exit(main())
