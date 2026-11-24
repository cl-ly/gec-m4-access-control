#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""截取门禁后台的关键界面, 供 README 与 docs/platform.md 引用.

本机 agent-desktop 在 headless 策略下拒绝物理输入, 所以截图走浏览器级
DevTools 协议. 两个已验证的坑:

  1. 通过 Target.attachToTarget 拿 session 再发 Page.enable 会超时, 所以改用
     Target.getTargets 拿 targetId, 直接拼页面级 ws
     (ws://127.0.0.1:<port>/devtools/page/<targetId>) 连上去;
  2. 只等 document.readyState == complete 会在新标签页的 about:blank 上
     立刻成立, 于是后续 fetch 从空 origin 发出被判跨域 (TypeError: Failed
     to fetch). 必须同时校验 location.href 已经切到目标地址.

登录也不再注入 fetch, 而是填页面自己的登录表单并派发 submit, 走应用真实
代码路径, 顺带验证前端本身能不能登录.

用法:
  DOOR_WEB_PASSWORD='后台密码' python3 shots.py \\
      --browser-ws ws://127.0.0.1:9334/devtools/browser/<uuid> \\
      --base http://<服务器IP> --outdir ../docs/shots

密码只从环境变量或 --password 读, 不落任何文件.
"""

import argparse
import base64
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

from cdp_browser import WS  # noqa: E402


def norm(url):
    return (url or "").split("#")[0].split("?")[0].rstrip("/")


class Page(object):
    """页面级 ws 上的最小操作集."""

    def __init__(self, ws):
        self.ws = ws

    def soft(self, method, params=None, timeout=15):
        try:
            self.ws.call(method, params or {}, timeout=timeout)
            return True
        except Exception as e:
            print("  (soft fail %s: %s)" % (method, e))
            return False

    def ev(self, expr, timeout=35):
        r = self.ws.call("Runtime.evaluate",
                         {"expression": expr, "returnByValue": True,
                          "awaitPromise": True}, timeout=timeout)
        if r.get("exceptionDetails"):
            raise RuntimeError(str(r["exceptionDetails"])[:300])
        return r.get("result", {}).get("value")

    def where(self):
        """一次取回 href 与 readyState, 避免两次往返之间的竞态."""
        raw = self.ev("JSON.stringify({h: location.href, r: document.readyState})",
                      timeout=10)
        try:
            return json.loads(raw) if raw else {}
        except ValueError:
            return {}

    def goto(self, url, timeout=40):
        want = norm(url)
        for attempt in range(3):
            self.soft("Page.navigate", {"url": url})
            end = time.time() + timeout
            while time.time() < end:
                try:
                    o = self.where()
                    if norm(o.get("h")) == want and o.get("r") == "complete":
                        return True
                except Exception:
                    pass
                time.sleep(0.4)
            print("  goto attempt %d not ready: %s" % (attempt + 1, url))
        return False

    def metrics(self, width, height, mobile=False):
        return self.soft("Emulation.setDeviceMetricsOverride",
                         {"width": width, "height": height,
                          "deviceScaleFactor": 1, "mobile": bool(mobile)})

    def shot(self, path):
        time.sleep(0.8)
        r = self.ws.call("Page.captureScreenshot",
                         {"format": "png", "captureBeyondViewport": False}, timeout=45)
        data = base64.b64decode(r.get("data") or "")
        d = os.path.dirname(path)
        if d and not os.path.isdir(d):
            os.makedirs(d, exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)
        return len(data)


def page_ws_url(browser_ws, want=""):
    """从浏览器级 ws 里挑一个页面 target, 返回它的页面级 ws 地址."""
    hostport = browser_ws[5:].split("/")[0]
    ws = WS(browser_ws, timeout=45)
    infos = ws.call("Target.getTargets", timeout=30).get("targetInfos", [])
    pages = [t for t in infos if t.get("type") == "page"]
    print("pages: %d" % len(pages))
    for t in pages:
        print("  %-46s %s" % ((t.get("url") or "")[:46], (t.get("title") or "")[:28]))

    pick = None
    if want:
        for t in pages:
            if want in (t.get("url") or ""):
                pick = t
                break
    if pick is None and pages:
        pick = pages[0]
    if pick is not None and pick.get("targetId"):
        tid = pick["targetId"]
    else:
        tid = ws.call("Target.createTarget", {"url": "about:blank"},
                      timeout=30).get("targetId")
    return "ws://%s/devtools/page/%s" % (hostport, tid)


# 填页面自己的登录表单再派发 submit, 走应用真实路径 (它内部自己 fetch).
LOGIN_JS = """(function(){
  var out = {href: location.origin, has_input: false, submitted: false};
  var i = document.getElementById('login-pwd');
  if (i) {
    out.has_input = true;
    i.value = %s;
    i.dispatchEvent(new Event('input', {bubbles: true}));
    var f = document.getElementById('login-form');
    if (f) {
      f.dispatchEvent(new Event('submit', {cancelable: true, bubbles: true}));
      out.submitted = true;
    }
  }
  return JSON.stringify(out);
})()"""

VIS_JS = """(function(){
  var app = document.getElementById('app');
  var login = document.getElementById('login-view');
  return JSON.stringify({app: app ? !app.hidden : null,
      login: login ? !login.hidden : null});
})()"""

STATE_JS = """(function(){
  var pill = document.getElementById('board-pill');
  var app = document.getElementById('app');
  return JSON.stringify({path: location.pathname,
      title: document.title,
      loggedIn: app ? !app.hidden : null,
      pill: pill ? pill.textContent : null,
      statCards: document.querySelectorAll('#stats .stat').length,
      eventRows: document.querySelectorAll('#events-wrap tbody tr').length,
      cardRows: document.querySelectorAll('#cards-wrap tbody tr').length,
      head: (document.body.innerText || '').replace(/\\s+/g, ' ').slice(0, 120)});
})()"""

SWITCH_JS = """(function(){
  var b = document.querySelector('.nav-btn[data-view=%s]');
  if (!b) return 'no-button';
  b.click();
  return 'clicked';
})()"""


def wait_logged_in(page, timeout=20):
    end = time.time() + timeout
    while time.time() < end:
        try:
            o = json.loads(page.ev(VIS_JS, timeout=10) or "{}")
            if o.get("app") is True:
                return True
            if o.get("login") is True:
                # 登录表单还在, 再提交一次 (可能第一次填值时脚本还没绑定)
                page.ev(LOGIN_JS % json.dumps(page.password))
        except Exception:
            pass
        time.sleep(0.5)
    return False


def main(argv=None):
    ap = argparse.ArgumentParser(description="截取门禁后台关键界面")
    ap.add_argument("--browser-ws", default="", help="浏览器级 ws, 用来自动挑页面")
    ap.add_argument("--page-ws", default="", help="页面级 ws, 给了就直连")
    ap.add_argument("--want", default="", help="优先选 url 含此串的页面")
    ap.add_argument("--base", default="", help="后台站点根, 例如 http://1.2.3.4")
    ap.add_argument("--path", default="/door/",
                    help="后台入口路径: 经 nginx 是 /door/, 直连 8081 是 /")
    ap.add_argument("--password", default=os.environ.get("DOOR_WEB_PASSWORD", ""))
    ap.add_argument("--outdir", default=os.path.join(HERE, "..", "docs", "shots"))
    ap.add_argument("--width", type=int, default=1440)
    ap.add_argument("--height", type=int, default=980)
    ap.add_argument("--skip-mobile", action="store_true", help="跳过手机视口那一张")
    a = ap.parse_args(argv)

    if not a.password:
        print("缺少后台密码: 设环境变量 DOOR_WEB_PASSWORD 或传 --password")
        return 2
    if not a.base:
        print("缺少 --base, 例如 --base http://1.2.3.4")
        return 2
    url = a.page_ws or (page_ws_url(a.browser_ws, a.want) if a.browser_ws else "")
    if not url:
        print("需要 --page-ws 或 --browser-ws")
        return 2
    print("page ws: %s" % url)

    outdir = os.path.abspath(a.outdir)
    base = a.base.rstrip("/")
    page = Page(WS(url, timeout=45))
    page.password = a.password
    page.soft("Page.enable")
    page.soft("Runtime.enable")
    page.metrics(a.width, a.height)

    home = base + (a.path if a.path.startswith("/") else ("/" + a.path))
    if not page.goto(home):
        print("导航失败, 中止")
        return 1
    print("load: %s" % json.dumps(page.where(), ensure_ascii=False))

    if not wait_logged_in(page):
        print("自动登录失败, 页面停在登录态")
        print("state: %s" % page.ev(STATE_JS))
        return 1
    print("login ok")

    time.sleep(2.5)
    state = page.ev(STATE_JS)
    print("state: %s" % state)
    if '"loggedIn":true' not in (state or ""):
        print("没能进入已登录态, 中止")
        return 1

    shots = []

    def take(name, title, js=None, wait=2.4):
        if js:
            print("  %s -> %s" % (name, page.ev(js)))
            time.sleep(wait)
        n = page.shot(os.path.join(outdir, name))
        shots.append({"file": name, "title": title, "bytes": n})
        print("  saved %s (%d bytes)" % (name, n))

    take("01-overview.png", "概览: 远程开门与板子状态", wait=0)
    take("02-events.png", "开门记录: 时间 类型 结果 UID 详情 来源",
         SWITCH_JS % json.dumps("events"), wait=2.8)
    take("03-cards.png", "门卡: 自动收录的 UID 与备注 启停 删除",
         SWITCH_JS % json.dumps("cards"), wait=1.8)
    take("04-settings.png", "设置: 修改后台密码",
         SWITCH_JS % json.dumps("settings"), wait=1.6)

    if not a.skip_mobile:
        page.metrics(390, 844, mobile=True)
        if page.goto(home):
            time.sleep(2.5)
            take("05-mobile.png", "手机视口: 同一后台在手机上的布局", wait=0)
        else:
            print("手机视口导航失败, 跳过")

    print(json.dumps({"outdir": outdir, "shots": shots}, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
