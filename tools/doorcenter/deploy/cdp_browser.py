#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""在浏览器级 DevTools 协议上驱动 Edge, 纯标准库.

为什么需要它: 本机 agent-desktop 处于 headless 模式, 物理输入 (鼠标坐标点击,
SendInput 按键) 一律被拒; 但浏览器调试端口仍然接受 Input.insertText 与
Input.dispatchKeyEvent, 于是可以在不移动光标的前提下把命令打进云控制台终端.

端口不靠猜: 从 Edge 的 DevToolsActivePort 文件读出端口与浏览器级 ws 路径,
再按路径直接连 /devtools/browser/<uuid>, 用 Target.getTargets 找页面.

用法:
  python cdp_browser.py list
  python cdp_browser.py eval --js "document.title"
  python cdp_browser.py type --text "echo hi" --enter --focus
  python cdp_browser.py key --key Enter

可选: --match 关键字 (默认 workbench), --port 覆盖自动探测, --wait 秒.
"""

import argparse
import base64
import json
import os
import socket
import struct
import sys
import time

EDGE_DATA = os.path.join(
    os.environ.get("LOCALAPPDATA", r"C:\Users\CHY\AppData\Local"),
    "Microsoft", "Edge", "User Data")


def active_port(data_dir=EDGE_DATA):
    path = os.path.join(data_dir, "DevToolsActivePort")
    with open(path, "r", encoding="utf-8") as f:
        lines = [x.strip() for x in f if x.strip()]
    if len(lines) < 2:
        raise RuntimeError("DevToolsActivePort 内容不完整: %r" % lines)
    return int(lines[0]), lines[1]


class WS(object):
    """最小 WebSocket 客户端, 只为 CDP 服务."""

    def __init__(self, url, timeout=25.0):
        if not url.startswith("ws://"):
            raise RuntimeError("只支持 ws:// : %s" % url)
        hostport, _, path = url[5:].partition("/")
        host, _, port = hostport.partition(":")
        self.sock = socket.create_connection((host, int(port or 80)), timeout=timeout)
        self.sock.settimeout(timeout)
        key = base64.b64encode(os.urandom(16)).decode("ascii")
        req = ("GET /%s HTTP/1.1\r\nHost: %s:%s\r\nUpgrade: websocket\r\n"
               "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
               "Sec-WebSocket-Version: 13\r\n\r\n") % (path, host, port or 80, key)
        self.sock.sendall(req.encode("ascii"))
        buf = b""
        while b"\r\n\r\n" not in buf:
            d = self.sock.recv(4096)
            if not d:
                raise RuntimeError("握手时对端关闭")
            buf += d
        head, _, self.buf = buf.partition(b"\r\n\r\n")
        first = head.split(b"\r\n")[0]
        if b" 101" not in first:
            raise RuntimeError("握手失败: %r" % first[:160])
        self.next_id = 0

    def _frame(self, payload, opcode=0x1):
        data = payload if isinstance(payload, bytes) else payload.encode("utf-8")
        mask = os.urandom(4)
        n = len(data)
        hdr = bytearray([0x80 | opcode])
        if n < 126:
            hdr.append(0x80 | n)
        elif n < 65536:
            hdr.append(0x80 | 126)
            hdr += struct.pack(">H", n)
        else:
            hdr.append(0x80 | 127)
            hdr += struct.pack(">Q", n)
        hdr += mask
        self.sock.sendall(bytes(hdr) + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

    def _exact(self, n):
        while len(self.buf) < n:
            d = self.sock.recv(max(4096, n - len(self.buf)))
            if not d:
                raise RuntimeError("连接已关闭")
            self.buf += d
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def _text(self):
        while True:
            b0, b1 = self._exact(2)
            op = b0 & 0x0F
            masked = b1 & 0x80
            ln = b1 & 0x7F
            if ln == 126:
                ln = struct.unpack(">H", self._exact(2))[0]
            elif ln == 127:
                ln = struct.unpack(">Q", self._exact(8))[0]
            mask = self._exact(4) if masked else None
            data = self._exact(ln)
            if mask:
                data = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
            if op == 0x9:
                self._frame(data, 0xA)
                continue
            if op == 0xA:
                continue
            if op == 0x8:
                raise RuntimeError("收到 close 帧")
            return data.decode("utf-8", "replace")

    def call(self, method, params=None, session=None, timeout=25.0):
        self.next_id += 1
        mid = self.next_id
        msg = {"id": mid, "method": method, "params": params or {}}
        if session:
            msg["sessionId"] = session
        self._frame(json.dumps(msg))
        end = time.time() + timeout
        while time.time() < end:
            rep = json.loads(self._text())
            if rep.get("id") == mid:
                if rep.get("error"):
                    raise RuntimeError("%s -> %s" % (method, json.dumps(rep["error"], ensure_ascii=False)[:300]))
                return rep.get("result", {})
        raise RuntimeError("等待 %s 超时" % method)


FOCUS_JS = """(function(){
  var sel = ['textarea.xterm-helper-textarea', 'textarea', '[contenteditable=true]'];
  for (var s = 0; s < sel.length; s++) {
    var list = document.querySelectorAll(sel[s]);
    for (var i = 0; i < list.length; i++) {
      var el = list[i], r = el.getBoundingClientRect();
      if (r.width > 0 && r.height > 0) {
        el.focus();
        return sel[s] + ' -> focused=' + (document.activeElement === el);
      }
    }
  }
  return 'no-focusable-input';
})()"""


KEYS = {
    "Enter": ("Enter", 13, "Enter"),
    "Tab": ("Tab", 9, "Tab"),
    "Backspace": ("Backspace", 8, "Backspace"),
    "Escape": ("Escape", 27, "Escape"),
}


def attach_page(ws, info):
    r = ws.call("Target.attachToTarget", {"targetId": info["targetId"], "flatten": True})
    return r.get("sessionId")


def main(argv=None):
    ap = argparse.ArgumentParser(description="浏览器级 CDP 客户端")
    ap.add_argument("action", choices=["list", "eval", "type", "key", "shot", "clear"])
    ap.add_argument("--js", default="")
    ap.add_argument("--text", default="")
    ap.add_argument("--key", default="Enter")
    ap.add_argument("--enter", action="store_true")
    ap.add_argument("--focus", action="store_true")
    ap.add_argument("--match", default="workbench")
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--wait", type=float, default=0.0)
    ap.add_argument("--out", default="", help="shot: 保存的 PNG 路径")
    ap.add_argument("--data-dir", default="",
                    help="覆盖 Edge 用户数据目录, 从它里面读 DevToolsActivePort")
    ap.add_argument("--ws", default="",
                    help="直接给浏览器级 ws 地址, 跳过端口探测 (新实例常不写 DevToolsActivePort)")
    ap.add_argument("--path", default="", help="配合 --port 指定 /devtools/browser/<uuid>")
    ap.add_argument("--width", type=int, default=0, help="shot: 视口宽, 0 表示不改")
    ap.add_argument("--height", type=int, default=0, help="shot: 视口高")
    ap.add_argument("--mobile", action="store_true", help="shot: 按移动端视口截")
    a = ap.parse_args(argv)

    if a.ws:
        url = a.ws
    else:
        port = a.port or 0
        path = a.path or ""
        if not (port and path):
            try:
                p2, path2 = active_port(a.data_dir or EDGE_DATA)
            except OSError:
                print("读不到 DevToolsActivePort, 请用 --ws 直连或 --port + --path")
                return 2
            port = port or p2
            path = path or path2
        url = "ws://127.0.0.1:%d%s" % (port, path)
    print("endpoint: %s" % url)
    ws = WS(url)

    targets = ws.call("Target.getTargets").get("targetInfos", [])
    usable = [t for t in targets if t.get("type") in ("page", "iframe", "webview")]
    if a.action == "list":
        for t in usable:
            print("%-7s %-30s %s" % (t.get("type"), (t.get("title") or "")[:30],
                                     (t.get("url") or "")[:95]))
        return 0

    key = a.match.lower()
    hits = [t for t in usable
            if key in ((t.get("url") or "") + " " + (t.get("title") or "")).lower()]
    if not hits:
        hits = usable
    if not hits:
        print("没有可用目标")
        return 1

    rc = 0
    for info in hits:
        label = ((info.get("title") or "")[:40], (info.get("url") or "")[:70])
        try:
            sid = attach_page(ws, info)
        except RuntimeError as e:
            print("attach 失败 %s: %s" % (label[1], e))
            rc = 1
            continue
        print("attached %s | %s" % (label[0], label[1]))
        try:
            if a.action == "eval":
                r = ws.call("Runtime.evaluate", {"expression": a.js, "returnByValue": True,
                                                 "awaitPromise": True}, session=sid)
                print("  value: %s" % json.dumps(r.get("result", {}).get("value"), ensure_ascii=False))
            elif a.action == "shot":
                if a.width and a.height:
                    ws.call("Emulation.setDeviceMetricsOverride",
                            {"width": a.width, "height": a.height,
                             "deviceScaleFactor": 1, "mobile": bool(a.mobile)}, session=sid)
                    time.sleep(0.4)
                if a.js:
                    ws.call("Runtime.evaluate", {"expression": a.js, "returnByValue": True,
                                                 "awaitPromise": True}, session=sid)
                    time.sleep(float(a.wait or 1.0))
                ws.call("Page.enable", {}, session=sid)
                r = ws.call("Page.captureScreenshot",
                            {"format": "png", "captureBeyondViewport": False}, session=sid)
                data = base64.b64decode(r.get("data") or "")
                out = a.out or ("shot-%d.png" % int(time.time()))
                with open(out, "wb") as f:
                    f.write(data)
                print("  saved %s (%d bytes)" % (out, len(data)))
            elif a.action == "clear":
                ws.call("Emulation.clearDeviceMetricsOverride", {}, session=sid)
                print("  metrics cleared")
            elif a.action == "key":
                k = KEYS.get(a.key, (a.key, 0, a.key))
                for kind in ("keyDown", "keyUp"):
                    ws.call("Input.dispatchKeyEvent", {"type": kind, "key": k[0], "code": k[2],
                                                      "windowsVirtualKeyCode": k[1],
                                                      "nativeVirtualKeyCode": k[1]}, session=sid)
                print("  key sent: %s" % a.key)
            else:
                if a.focus:
                    r = ws.call("Runtime.evaluate", {"expression": FOCUS_JS, "returnByValue": True},
                                session=sid)
                    print("  focus: %s" % r.get("result", {}).get("value"))
                ws.call("Input.insertText", {"text": a.text}, session=sid)
                print("  inserted %d chars" % len(a.text))
                if a.enter:
                    k = KEYS["Enter"]
                    for kind in ("keyDown", "keyUp"):
                        ws.call("Input.dispatchKeyEvent", {"type": kind, "key": k[0], "code": k[2],
                                                          "windowsVirtualKeyCode": k[1],
                                                          "nativeVirtualKeyCode": k[1]}, session=sid)
                    print("  enter sent")
        except RuntimeError as e:
            print("  失败: %s" % e)
            rc = 1
    if a.wait:
        time.sleep(a.wait)
    return rc


if __name__ == "__main__":
    sys.exit(main())
