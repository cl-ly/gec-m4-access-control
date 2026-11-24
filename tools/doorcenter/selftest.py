#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""doorcenter self test.

Starts the service on throwaway ports, plays the role of the board (ESP8266)
with a fake TCP client, drives the HTTP API like the web UI does, and checks
that every event really landed in SQLite.

Run:  python selftest.py
"""

import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
PY = sys.executable
LEASE = 19080
HTTP = 19081
PWD = "selftest-123456"

fails = []


def check(name, cond, extra=""):
    print(("PASS " if cond else "FAIL ") + name + ((" | " + str(extra)) if extra and not cond else ""))
    if not cond:
        fails.append(name)
    return cond


def wait_port(port, timeout=12.0, host="127.0.0.1"):
    end = time.time() + timeout
    while time.time() < end:
        try:
            s = socket.create_connection((host, port), timeout=0.5)
            s.close()
            return True
        except OSError:
            time.sleep(0.15)
    return False


def http(method, path, body=None, sid=None):
    url = "http://127.0.0.1:%d%s" % (HTTP, path)
    data = None
    headers = {}
    if body is not None:
        data = json.dumps(body).encode("utf-8")
        headers["Content-Type"] = "application/json"
    if sid:
        headers["Cookie"] = "dc_sid=" + sid
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=8) as r:
            raw = r.read().decode("utf-8", "replace")
            ctype = r.headers.get("Content-Type", "") or ""
            if "application/json" in ctype:
                obj = json.loads(raw) if raw else {}
            else:
                obj = {"_raw": raw}
            return r.status, obj, r.headers
    except urllib.error.HTTPError as e:
        raw = e.read().decode("utf-8")
        try:
            obj = json.loads(raw) if raw else {}
        except ValueError:
            obj = {"error": raw}
        return e.code, obj, e.headers


class Board:
    """Fake ESP8266: same line protocol the firmware speaks."""

    def __init__(self):
        self.sock = socket.create_connection(("127.0.0.1", LEASE), timeout=5)
        self.sock.settimeout(6)
        self.buf = b""

    def send(self, text):
        self.sock.sendall(text.encode("ascii"))

    def read_line(self, timeout=6.0):
        end = time.time() + timeout
        while b"\n" not in self.buf:
            if time.time() > end:
                return ""
            try:
                d = self.sock.recv(512)
            except socket.timeout:
                return ""
            if not d:
                return ""
            self.buf += d
        line, self.buf = self.buf.split(b"\n", 1)
        return line.decode("ascii", "replace").strip()

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def main():
    tmp = tempfile.mkdtemp(prefix="doorcenter-selftest-")
    db = os.path.join(tmp, "door.db")
    proc = subprocess.Popen(
        [PY, os.path.join(HERE, "doorcenter.py"),
         "--db", db, "--ui", os.path.join(HERE, "ui"),
         "--board-host", "127.0.0.1", "--board-port", str(LEASE),
         "--http-host", "127.0.0.1", "--http-port", str(HTTP),
         "--password", PWD],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    try:
        if not wait_port(LEASE) or not wait_port(HTTP):
            proc.kill()
            out = proc.stdout.read().decode("utf-8", "replace")
            print("service did not start\n" + out)
            return 1

        # ---- static + unauthenticated ----
        st, _, _ = http("GET", "/")
        check("index.html served", st == 200, st)
        st, _, _ = http("GET", "/app.css")
        check("app.css served", st == 200, st)
        st, _, _ = http("GET", "/app.js")
        check("app.js served", st == 200, st)
        st, body, _ = http("GET", "/api/state")
        check("state requires login", st == 401, (st, body))
        st, body, _ = http("GET", "/api/health")
        check("health public", st == 200 and body.get("ok") is True, (st, body))

        # ---- login ----
        st, body, _ = http("POST", "/api/login", {"password": "wrong-one"})
        check("bad login rejected", st == 401, (st, body))
        st, body, hdrs = http("POST", "/api/login", {"password": PWD})
        setc = hdrs.get("Set-Cookie", "") or ""
        sid = ""
        for part in setc.split(";"):
            part = part.strip()
            if part.startswith("dc_sid="):
                sid = part[len("dc_sid="):]
        check("login ok with cookie", st == 200 and sid != "", (st, setc))

        # ---- board side ----
        b = Board()
        b.send("PING\n")
        check("board gets PONG", b.read_line() == "PONG")

        b.send("CARD,9A8E7A0668,OK\n")
        b.send("CARD,DEADBEEF01,DENY\n")
        b.send("PWD,OK\n")
        b.send("PWD,DENY\n")
        b.send("PWD,LOCK\n")
        time.sleep(0.8)

        st, body, _ = http("GET", "/api/state", sid=sid)
        check("state readable when logged in", st == 200, (st, body))
        check("board counted online", body.get("online") == 1, body.get("online"))
        stats = body.get("stats") or {}
        check("card OK recorded", stats.get("today_card_ok") == 1, stats)
        check("card DENY recorded", stats.get("today_card_deny") == 1, stats)
        check("password OK recorded", stats.get("today_pwd_ok") == 1, stats)
        check("password DENY recorded", stats.get("today_pwd_deny") == 1, stats)
        check("password LOCK recorded", stats.get("today_pwd_lock") == 1, stats)
        uids = [c.get("uid") for c in (body.get("cards") or [])]
        check("card uid auto collected", "9A8E7A0668" in uids, uids)

        evs = body.get("events") or []
        card_ev = [e for e in evs if e.get("kind") == "card" and e.get("uid") == "9A8E7A0668"]
        check("card event has timestamp", bool(card_ev) and len(card_ev[0].get("ts", "")) == 19,
              card_ev[:1])

        # ---- remote open round trip ----
        st, body, _ = http("POST", "/api/open", {}, sid=sid)
        check("open accepted", st == 200 and body.get("sent") == 1, (st, body))
        line = b.read_line()
        check("board received OPEN", line == "OPEN", line)
        b.send("OK\n")
        time.sleep(0.8)

        st, body, _ = http("GET", "/api/events?limit=50", sid=sid)
        rem = [e for e in (body.get("events") or []) if e.get("kind") == "remote"]
        kinds = [(e.get("result"), e.get("latency_ms")) for e in rem]
        check("remote SENT logged", any(k[0] == "SENT" for k in kinds), kinds)
        check("remote OK logged with latency", any(k[0] == "OK" and k[1] >= 0 for k in kinds), kinds)

        # ---- open with no board ----
        b.close()
        time.sleep(1.2)
        st, body, _ = http("POST", "/api/open", {}, sid=sid)
        check("open refused when board offline", st == 409, (st, body))

        # ---- cards admin ----
        st, body, _ = http("POST", "/api/cards/update",
                           {"uid": "9A8E7A0668", "label": "gate main", "enabled": True}, sid=sid)
        check("card label saved", st == 200, (st, body))
        st, body, _ = http("GET", "/api/state", sid=sid)
        got = [c for c in (body.get("cards") or []) if c.get("uid") == "9A8E7A0668"]
        check("card label persisted", bool(got) and got[0].get("label") == "gate main", got[:1])

        # ---- password change ----
        st, body, _ = http("POST", "/api/password", {"old": "nope", "new": "abcdef123"}, sid=sid)
        check("password change needs old", st == 400, (st, body))
        st, body, _ = http("POST", "/api/password", {"old": PWD, "new": "abcdef123"}, sid=sid)
        check("password changed", st == 200, (st, body))
        st, body, _ = http("GET", "/api/state", sid=sid)
        check("old session dropped after change", st == 401, (st, body))
        st, body, hdrs = http("POST", "/api/login", {"password": "abcdef123"})
        check("new password works", st == 200, (st, body))

        # ---- restart must keep the password ----
        proc.terminate()
        proc.wait(timeout=10)
        proc = subprocess.Popen(
            [PY, os.path.join(HERE, "doorcenter.py"),
             "--db", db, "--ui", os.path.join(HERE, "ui"),
             "--board-host", "127.0.0.1", "--board-port", str(LEASE),
             "--http-host", "127.0.0.1", "--http-port", str(HTTP)],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if not wait_port(HTTP):
            check("restart ok", False, "http not up")
        else:
            st, body, _ = http("POST", "/api/login", {"password": "abcdef123"})
            check("password survives restart without --password", st == 200, (st, body))
            st, body, _ = http("POST", "/api/login", {"password": PWD})
            check("initial password no longer works", st == 401, (st, body))

    finally:
        try:
            proc.terminate()
            proc.wait(timeout=10)
        except Exception:
            try:
                proc.kill()
            except Exception:
                pass
        shutil.rmtree(tmp, ignore_errors=True)

    print("")
    if fails:
        print("FAILED: %d check(s): %s" % (len(fails), ", ".join(fails)))
        return 1
    print("ALL CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
