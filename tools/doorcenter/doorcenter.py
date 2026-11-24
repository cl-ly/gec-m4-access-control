#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""门禁中控服务 doorcenter.

一个进程同时干三件事:
  1) 8080 端口 TCP 网关: 承接板子(ESP8266)的长连接, 协议与原 v4 服务端逐字节兼容
     - 上行 PING                -> 回 PONG (板子据此判定这条连接双向都通)
     - 上行 CARD,<uid>,OK|DENY  -> 记一条刷卡记录
     - 上行 PWD,OK|DENY|LOCK    -> 记一条密码验证记录
     - 下行 OPEN                -> 板子开锁, 开锁成功后回 OK
  2) 8081 端口 HTTP 后台: 网页登录后远程开门 / 查记录 / 管门卡 / 改后台密码
  3) SQLite 落库: 所有事件带服务器本地时间 (Asia/Shanghai)

运行:
  python3 doorcenter.py --db /var/lib/doorcenter/door.db --ui ./ui \
      --board-port 8080 --http-host 127.0.0.1 --http-port 8081

只重设后台密码:
  python3 doorcenter.py --db ... --password '新的后台密码' --set-password
"""

import argparse
import hashlib
import hmac
import json
import os
import socket
import sqlite3
import sys
import threading
import time
from datetime import datetime, timedelta, timezone
from http.cookies import SimpleCookie
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

VER = "v5"
ONLINE_WINDOW = 90.0      # 板子超过这么久没数据就算掉线
SESSION_TTL = 12 * 3600   # 后台登录有效期
LOGIN_MAX_FAIL = 5        # 连续失败几次开始锁
LOGIN_LOCK_SEC = 120      # 锁多久
MAX_BODY = 64 * 1024
OPEN_ACK_WAIT = 4.0       # 远程开门等板子回执 OK 的上限
OPEN_TARGET_AGE = 25.0    # 只把 OPEN 下发给最近 25 s 内有数据的连接 (板子每 10 s 一条 PING)


def _load_tz():
    try:
        from zoneinfo import ZoneInfo
        return ZoneInfo("Asia/Shanghai")
    except Exception:
        return timezone(timedelta(hours=8))


TZ = _load_tz()


def now_dt():
    return datetime.now(TZ)


def now_str():
    return now_dt().strftime("%Y-%m-%d %H:%M:%S")


def stamp(msg):
    print("%s %s" % (now_str(), msg), flush=True)


SCHEMA = """
CREATE TABLE IF NOT EXISTS events (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  ts TEXT NOT NULL,
  epoch REAL NOT NULL,
  kind TEXT NOT NULL,
  result TEXT NOT NULL,
  uid TEXT NOT NULL DEFAULT '',
  detail TEXT NOT NULL DEFAULT '',
  ip TEXT NOT NULL DEFAULT '',
  latency_ms INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_events_epoch ON events(epoch DESC);
CREATE TABLE IF NOT EXISTS cards (
  uid TEXT PRIMARY KEY,
  label TEXT NOT NULL DEFAULT '',
  enabled INTEGER NOT NULL DEFAULT 1,
  added_at TEXT NOT NULL DEFAULT '',
  last_seen TEXT NOT NULL DEFAULT ''
);
CREATE TABLE IF NOT EXISTS settings (
  k TEXT PRIMARY KEY,
  v TEXT NOT NULL
);
"""


class Store:
    """SQLite 落库, 每个操作短连接, 用一把锁串行化写入."""

    def __init__(self, path):
        self.path = path
        d = os.path.dirname(os.path.abspath(path))
        if d and not os.path.isdir(d):
            os.makedirs(d, exist_ok=True)
        self.lock = threading.Lock()
        c = self._conn()
        try:
            c.executescript(SCHEMA)
            c.commit()
        finally:
            c.close()

    def _conn(self):
        c = sqlite3.connect(self.path, timeout=15)
        c.row_factory = sqlite3.Row
        c.execute("PRAGMA journal_mode=WAL")
        c.execute("PRAGMA synchronous=NORMAL")
        return c

    def add_event(self, kind, result, uid="", detail="", ip="", latency_ms=0):
        ts = now_str()
        ep = time.time()
        with self.lock:
            c = self._conn()
            try:
                cur = c.execute(
                    "INSERT INTO events(ts,epoch,kind,result,uid,detail,ip,latency_ms)"
                    " VALUES(?,?,?,?,?,?,?,?)",
                    (ts, ep, kind, result, uid, detail, ip, int(latency_ms)))
                c.commit()
                return cur.lastrowid
            finally:
                c.close()

    def events(self, limit=50, kind=""):
        q = "SELECT * FROM events"
        args = []
        if kind:
            q += " WHERE kind=?"
            args.append(kind)
        q += " ORDER BY id DESC LIMIT ?"
        args.append(int(limit))
        with self.lock:
            c = self._conn()
            try:
                rows = c.execute(q, args).fetchall()
            finally:
                c.close()
        return [dict(r) for r in rows]

    def stats(self):
        day0 = now_dt().replace(hour=0, minute=0, second=0, microsecond=0).timestamp()
        with self.lock:
            c = self._conn()
            try:
                def one(sql, args=()):
                    r = c.execute(sql, args).fetchone()
                    return int(r[0]) if r and r[0] is not None else 0

                return {
                    "today_card_ok": one(
                        "SELECT COUNT(*) FROM events WHERE kind='card' AND result='OK' AND epoch>=?",
                        (day0,)),
                    "today_card_deny": one(
                        "SELECT COUNT(*) FROM events WHERE kind='card' AND result<>'OK' AND epoch>=?",
                        (day0,)),
                    "today_pwd_ok": one(
                        "SELECT COUNT(*) FROM events WHERE kind='password' AND result='OK' AND epoch>=?",
                        (day0,)),
                    "today_pwd_deny": one(
                        "SELECT COUNT(*) FROM events WHERE kind='password' AND result='DENY' AND epoch>=?",
                        (day0,)),
                    "today_pwd_lock": one(
                        "SELECT COUNT(*) FROM events WHERE kind='password' AND result='LOCK' AND epoch>=?",
                        (day0,)),
                    "today_remote": one(
                        "SELECT COUNT(*) FROM events WHERE kind='remote' AND result='OK' AND epoch>=?",
                        (day0,)),
                    "total_events": one("SELECT COUNT(*) FROM events"),
                }
            finally:
                c.close()

    def cards(self):
        with self.lock:
            c = self._conn()
            try:
                rows = c.execute(
                    "SELECT * FROM cards ORDER BY last_seen DESC, uid ASC").fetchall()
            finally:
                c.close()
        return [dict(r) for r in rows]

    def card_seen(self, uid):
        ts = now_str()
        with self.lock:
            c = self._conn()
            try:
                c.execute(
                    "INSERT INTO cards(uid,label,enabled,added_at,last_seen) VALUES(?,?,?,?,?)"
                    " ON CONFLICT(uid) DO UPDATE SET last_seen=excluded.last_seen",
                    (uid, "", 1, ts, ts))
                c.commit()
            finally:
                c.close()

    def card_update(self, uid, label=None, enabled=None):
        with self.lock:
            c = self._conn()
            try:
                if label is not None:
                    c.execute("UPDATE cards SET label=? WHERE uid=?", (label, uid))
                if enabled is not None:
                    c.execute("UPDATE cards SET enabled=? WHERE uid=?",
                              (1 if enabled else 0, uid))
                c.commit()
            finally:
                c.close()

    def card_delete(self, uid):
        with self.lock:
            c = self._conn()
            try:
                c.execute("DELETE FROM cards WHERE uid=?", (uid,))
                c.commit()
            finally:
                c.close()

    def get_setting(self, k, default=""):
        with self.lock:
            c = self._conn()
            try:
                r = c.execute("SELECT v FROM settings WHERE k=?", (k,)).fetchone()
            finally:
                c.close()
        return r["v"] if r else default

    def set_setting(self, k, v):
        with self.lock:
            c = self._conn()
            try:
                c.execute("INSERT INTO settings(k,v) VALUES(?,?)"
                          " ON CONFLICT(k) DO UPDATE SET v=excluded.v", (k, v))
                c.commit()
            finally:
                c.close()


class Auth:
    """后台密码与登录会话.

    密码不存数据库, 而是单独一份凭据文件 (默认与数据库同目录的
    doorcenter-auth.json). 于是清库、换库、重新部署代码都不会把密码
    打回初始值, 只有显式 --set-password 才会改动它.
    文件里只有 pbkdf2 摘要与盐, 没有明文; 会话只存在内存.
    """

    def __init__(self, auth_file):
        self.path = auth_file
        self.lock = threading.Lock()
        self.sessions = {}
        self.fails = {}
        d = os.path.dirname(os.path.abspath(auth_file))
        if d and not os.path.isdir(d):
            os.makedirs(d, exist_ok=True)
        self.data = self._load()

    def _load(self):
        try:
            with open(self.path, "r", encoding="utf-8") as f:
                obj = json.load(f)
            if isinstance(obj, dict) and obj.get("pwd_salt") and obj.get("pwd_hash"):
                return obj
        except (OSError, ValueError):
            pass
        return {}

    def exists(self):
        return bool(self.data)

    def _save(self):
        tmp = self.path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(self.data, f, ensure_ascii=False, indent=2)
        try:
            os.chmod(tmp, 0o600)
        except OSError:
            pass
        os.replace(tmp, self.path)

    @staticmethod
    def _hash(pwd, salt_hex):
        return hashlib.pbkdf2_hmac("sha256", pwd.encode("utf-8"),
                                   bytes.fromhex(salt_hex), 200000).hex()

    def set_password(self, pwd):
        salt = os.urandom(16).hex()
        self.data = {
            "pwd_salt": salt,
            "pwd_hash": self._hash(pwd, salt),
            "updated": now_str(),
        }
        self._save()
        with self.lock:
            self.sessions.clear()

    def verify(self, pwd):
        salt = self.data.get("pwd_salt")
        expect = self.data.get("pwd_hash")
        if not salt or not expect:
            return False
        return hmac.compare_digest(self._hash(pwd, salt), expect)

    def new_session(self):
        tok = os.urandom(24).hex()
        with self.lock:
            self.sessions[tok] = time.time() + SESSION_TTL
        return tok

    def check(self, tok):
        if not tok:
            return False
        with self.lock:
            exp = self.sessions.get(tok)
            if not exp:
                return False
            if exp < time.time():
                self.sessions.pop(tok, None)
                return False
            return True

    def drop(self, tok):
        with self.lock:
            self.sessions.pop(tok, None)

    def throttled(self, ip):
        with self.lock:
            rec = self.fails.get(ip)
            if not rec:
                return 0
            left = rec[1] - time.time()
            return int(left) if left > 0 else 0

    def fail(self, ip):
        with self.lock:
            fails, until = self.fails.get(ip, (0, 0.0))
            if until < time.time():
                fails = 0
            fails += 1
            until = (time.time() + LOGIN_LOCK_SEC) if fails >= LOGIN_MAX_FAIL else 0.0
            self.fails[ip] = (fails, until)

    def clear_fails(self, ip):
        with self.lock:
            self.fails.pop(ip, None)


class BoardConn:
    def __init__(self, sock, addr):
        self.sock = sock
        self.addr = addr
        self.since = now_str()
        self.last_seen = time.time()


class Registry:
    """板子连接表."""

    def __init__(self):
        self.lock = threading.Lock()
        self.items = []
        # 待下发 OPEN 的连接: 板子每条 PING 都会上传, 在回 PONG 的同一批字节里
        # 顺带补一条 OPEN, 投递窗口最可靠 (固件的逐字节匹配不会被 SEND 等待吞掉).
        self.pending = set()

    def add(self, sock, addr):
        c = BoardConn(sock, addr)
        with self.lock:
            self.items.append(c)
        return c

    def drop(self, c):
        with self.lock:
            if c in self.items:
                self.items.remove(c)
        try:
            c.sock.close()
        except OSError:
            pass

    def touch(self, c):
        c.last_seen = time.time()

    def online(self):
        now = time.time()
        with self.lock:
            return [c for c in self.items if (now - c.last_seen) <= ONLINE_WINDOW]

    def snapshot(self):
        now = time.time()
        with self.lock:
            items = list(self.items)
        out = []
        for c in items:
            age = int(now - c.last_seen)
            out.append({
                "peer": "%s:%d" % (c.addr[0], c.addr[1]),
                "since": c.since,
                "age": age,
                "online": age <= ONLINE_WINDOW,
            })
        return out

    def push(self, payload, max_age=None):
        n = 0
        dead = []
        now = time.time()
        for c in self.online():
            if (max_age is not None) and ((now - c.last_seen) > max_age):
                continue
            try:
                c.sock.sendall(payload)
                n += 1
                stamp("sent %r to board %s:%d" % (payload, c.addr[0], c.addr[1]))
            except OSError:
                dead.append(c)
        for c in dead:
            self.drop(c)
        return n

    def queue_open(self, max_age=OPEN_TARGET_AGE):
        """把 OPEN 记在活跃连接上, 等它下一次 PING 时随 PONG 一起下发."""
        now = time.time()
        with self.lock:
            items = [c for c in self.items if (now - c.last_seen) <= max_age]
            for c in items:
                self.pending.add(c)
            return len(items)

    def take_pending(self, c):
        with self.lock:
            if c in self.pending:
                self.pending.remove(c)
                return True
            return False

    def clear_pending(self):
        with self.lock:
            self.pending.clear()


class Gateway:
    """板子协议解析: 一行一条消息, 与原 v4 服务端一致."""

    def __init__(self, store, reg, verbose=False):
        self.store = store
        self.reg = reg
        self.lock = threading.Lock()
        self.last_open = 0.0
        self.verbose = bool(verbose)
        self.ack_evt = threading.Event()

    def status_text(self):
        lines = ["STATUS", "ver=%s" % VER, "board=%d" % len(self.reg.online())]
        for p in self.reg.snapshot():
            lines.append("peer %s since %s age=%ds %s" % (
                p["peer"], p["since"], p["age"], "ONLINE" if p["online"] else "STALE"))
        return "\n".join(lines) + "\n"

    def open_door(self, source, ip=""):
        self.ack_evt.clear()
        # 两条投递路径: 立刻发一次, 同时记一笔"待补发", 由板子下一次 PING 的
        # PONG 回包捎带. 只要任一条生效, 板子开门后回 OK, 就清掉待补发.
        queued = self.reg.queue_open()
        n = self.reg.push(b"OPEN\n", max_age=OPEN_TARGET_AGE)
        with self.lock:
            self.last_open = time.time() if (n or queued) else 0.0
        got = self.ack_evt.wait(OPEN_ACK_WAIT) if (n or queued) else False
        self.store.add_event("remote", "SENT" if (n or queued) else "FAIL",
                             detail="source=%s immediate=%d queued=%d ack=%s"
                                    % (source, n, queued, "yes" if got else "pending"),
                             ip=ip)
        stamp("remote open by %s -> immediate %d, queued %d, ack %s"
              % (source, n, queued, "yes" if got else "pending"))
        return max(n, queued)

    def handle(self, conn, raw):
        line = raw.decode("ascii", "replace").strip()
        if not line:
            return
        up = line.upper()
        ip = conn.addr[0]
        if self.verbose:
            stamp("board line %s <- %r" % (ip, line[:80]))
        if up == "PING":
            # 板子每次心跳都在读, 不在发, 这是最可靠的投递窗口
            if self.reg.take_pending(conn):
                self._send(conn, b"PONG\nOPEN\n")
                stamp("queued OPEN delivered with PONG to %s" % ip)
            else:
                self._send(conn, b"PONG\n")
            return
        if up == "STATUS":
            self._send(conn, self.status_text().encode("ascii", "replace"))
            return
        if up in ("OPEN", "OPENNOW"):
            n = self.open_door("tcp", ip)
            self._send(conn, ("SENT %d\n" % n).encode("ascii"))
            return
        if up.startswith("CARD"):
            parts = line.split(",")
            uid = parts[1].strip().upper() if len(parts) > 1 else ""
            res = parts[2].strip().upper() if len(parts) > 2 else "?"
            if uid and all(ch in "0123456789ABCDEF" for ch in uid):
                self.store.card_seen(uid)
            self.store.add_event("card", res, uid=uid, detail="uid=%s" % uid, ip=ip)
            stamp("card %s %s" % (uid or "?", res))
            return
        if up.startswith("PWD"):
            parts = line.split(",")
            res = parts[1].strip().upper() if len(parts) > 1 else "?"
            self.store.add_event("password", res, detail="password auth", ip=ip)
            stamp("password %s" % res)
            return
        if up == "OK":
            self.ack_evt.set()
            self.reg.clear_pending()
            latency = 0
            with self.lock:
                if self.last_open:
                    latency = int((time.time() - self.last_open) * 1000)
                    self.last_open = 0.0
            self.store.add_event("remote", "OK", detail="board confirmed open",
                                 ip=ip, latency_ms=latency)
            stamp("board confirmed open (%d ms)" % latency)
            return
        self.store.add_event("system", "INFO", detail=line[:120], ip=ip)

    @staticmethod
    def _send(conn, payload):
        try:
            conn.sock.sendall(payload)
        except OSError:
            pass


class BoardServer(threading.Thread):
    daemon = True

    def __init__(self, host, port, gw, reg):
        threading.Thread.__init__(self, daemon=True)
        self.host = host
        self.port = port
        self.gw = gw
        self.reg = reg

    def run(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind((self.host, self.port))
        s.listen(16)
        stamp("board gateway listening on %s:%d" % (self.host, self.port))
        while True:
            try:
                c, a = s.accept()
            except OSError:
                return
            threading.Thread(target=self._serve, args=(c, a), daemon=True).start()

    def _serve(self, c, a):
        conn = self.reg.add(c, a)
        stamp("board connected from %s:%d" % (a[0], a[1]))
        self.gw.store.add_event("system", "INFO", detail="board connected", ip=a[0])
        c.settimeout(1.0)
        buf = b""
        try:
            while True:
                try:
                    d = c.recv(512)
                except socket.timeout:
                    continue
                except OSError:
                    break
                if not d:
                    break
                self.reg.touch(conn)
                buf += d
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    self.gw.handle(conn, line)
                if len(buf) > 2048:
                    buf = b""
        finally:
            self.reg.drop(conn)
            self.gw.store.add_event("system", "INFO", detail="board disconnected", ip=a[0])
            stamp("board disconnected %s:%d" % (a[0], a[1]))


class App:
    def __init__(self, store, auth, reg, gw, ui_dir, base=""):
        self.store = store
        self.auth = auth
        self.reg = reg
        self.gw = gw
        self.ui_dir = ui_dir
        self.base = base


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "doorcenter/" + VER
    app = None

    # ---- 基础设施 ----

    def log_message(self, fmt, *args):
        return

    def _send(self, code, body, ctype="application/json; charset=utf-8", extra=None):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header(
            "Content-Security-Policy",
            "default-src 'self'; img-src 'self' data:; style-src 'self'; "
            "script-src 'self'; connect-src 'self'; object-src 'none'; "
            "base-uri 'none'; frame-ancestors 'none'")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if body:
            self.wfile.write(body)

    def _json(self, code, obj, extra=None):
        self._send(code, json.dumps(obj, ensure_ascii=False), extra=extra)

    def _cookie(self, name):
        ck = SimpleCookie()
        try:
            ck.load(self.headers.get("Cookie", ""))
        except Exception:
            return ""
        m = ck.get(name)
        return m.value if m else ""

    def _authed(self):
        return self.app.auth.check(self._cookie("dc_sid"))

    def _path(self):
        p = urlparse(self.path).path
        base = self.app.base
        if base and p.startswith(base):
            p = p[len(base):] or "/"
        return p

    def _body(self):
        try:
            n = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            n = 0
        if n <= 0 or n > MAX_BODY:
            return {}
        raw = self.rfile.read(n)
        try:
            obj = json.loads(raw.decode("utf-8"))
        except Exception:
            return {}
        return obj if isinstance(obj, dict) else {}

    def _static(self, name, ctype):
        path = os.path.join(self.app.ui_dir, name)
        try:
            with open(path, "rb") as f:
                data = f.read()
        except OSError:
            return self._send(404, "missing " + name, "text/plain; charset=utf-8")
        return self._send(200, data, ctype)

    # ---- 路由 ----

    def do_GET(self):
        p = self._path()
        qs = parse_qs(urlparse(self.path).query)
        if p in ("/", "/index.html"):
            return self._static("index.html", "text/html; charset=utf-8")
        if p == "/app.css":
            return self._static("app.css", "text/css; charset=utf-8")
        if p == "/app.js":
            return self._static("app.js", "application/javascript; charset=utf-8")
        if p == "/favicon.ico":
            return self._send(204, b"", "image/x-icon")
        if p == "/api/health":
            return self._json(200, {"ok": True, "ver": VER,
                                    "board": len(self.app.reg.online()),
                                    "time": now_str()})
        if p == "/api/session":
            return self._json(200, {"authed": self._authed()})
        if not self._authed():
            return self._json(401, {"error": "请先登录"})
        if p == "/api/state":
            try:
                limit = int((qs.get("limit") or ["50"])[0])
            except ValueError:
                limit = 50
            limit = max(1, min(limit, 200))
            return self._json(200, {
                "ver": VER,
                "time": now_str(),
                "online": len(self.app.reg.online()),
                "board": self.app.reg.snapshot(),
                "events": self.app.store.events(limit),
                "stats": self.app.store.stats(),
                "cards": self.app.store.cards(),
            })
        if p == "/api/events":
            try:
                limit = int((qs.get("limit") or ["100"])[0])
            except ValueError:
                limit = 100
            limit = max(1, min(limit, 500))
            kind = (qs.get("kind") or [""])[0]
            if kind not in ("", "card", "password", "remote", "system", "admin"):
                kind = ""
            return self._json(200, {"events": self.app.store.events(limit, kind)})
        return self._json(404, {"error": "not found"})

    def do_POST(self):
        p = self._path()
        body = self._body()
        if p == "/api/login":
            return self._login(body)
        if p == "/api/logout":
            self.app.auth.drop(self._cookie("dc_sid"))
            return self._json(200, {"ok": True},
                              extra={"Set-Cookie":
                                     "dc_sid=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax"})
        if not self._authed():
            return self._json(401, {"error": "请先登录"})
        if p == "/api/open":
            ip = self.client_address[0]
            n = self.app.gw.open_door("web", ip)
            if n == 0:
                return self._json(409, {"error": "门禁板当前不在线, 指令未能下发", "sent": 0})
            return self._json(200, {"ok": True, "sent": n, "time": now_str()})
        if p == "/api/cards/update":
            uid = str(body.get("uid") or "").strip().upper()
            if not uid:
                return self._json(400, {"error": "缺少 uid"})
            label = body.get("label")
            enabled = body.get("enabled")
            self.app.store.card_update(
                uid,
                label=None if label is None else str(label)[:40],
                enabled=None if enabled is None else bool(enabled))
            return self._json(200, {"ok": True})
        if p == "/api/cards/delete":
            uid = str(body.get("uid") or "").strip().upper()
            if not uid:
                return self._json(400, {"error": "缺少 uid"})
            self.app.store.card_delete(uid)
            self.app.store.add_event("admin", "INFO", detail="card deleted %s" % uid,
                                     ip=self.client_address[0])
            return self._json(200, {"ok": True})
        if p == "/api/password":
            old = str(body.get("old") or "")
            new = str(body.get("new") or "")
            if not self.app.auth.verify(old):
                return self._json(400, {"error": "原密码不正确"})
            if len(new) < 6:
                return self._json(400, {"error": "新密码至少 6 位"})
            self.app.auth.set_password(new)
            self.app.store.add_event("admin", "INFO", detail="web password changed",
                                     ip=self.client_address[0])
            return self._json(200, {"ok": True, "relogin": True})
        return self._json(404, {"error": "not found"})

    def _login(self, body):
        ip = self.client_address[0]
        left = self.app.auth.throttled(ip)
        if left:
            return self._json(429, {"error": "尝试过多, 请 %d 秒后再试" % left})
        pwd = str(body.get("password") or "")
        if not pwd:
            return self._json(400, {"error": "请输入后台密码"})
        if not self.app.auth.verify(pwd):
            self.app.auth.fail(ip)
            self.app.store.add_event("admin", "DENY", detail="login failed", ip=ip)
            return self._json(401, {"error": "密码不正确"})
        self.app.auth.clear_fails(ip)
        tok = self.app.auth.new_session()
        self.app.store.add_event("admin", "OK", detail="login ok", ip=ip)
        return self._json(200, {"ok": True}, extra={"Set-Cookie":
                          "dc_sid=%s; Path=/; Max-Age=%d; HttpOnly; SameSite=Lax"
                          % (tok, SESSION_TTL)})


def main(argv=None):
    ap = argparse.ArgumentParser(description="门禁中控服务 doorcenter")
    here = os.path.dirname(os.path.abspath(__file__))
    ap.add_argument("--db", default=os.environ.get("DOOR_DB", "/var/lib/doorcenter/door.db"))
    ap.add_argument("--ui", default=os.environ.get("DOOR_UI", os.path.join(here, "ui")))
    ap.add_argument("--board-host", default="0.0.0.0")
    ap.add_argument("--board-port", type=int, default=8080)
    ap.add_argument("--http-host", default="127.0.0.1")
    ap.add_argument("--http-port", type=int, default=8081)
    ap.add_argument("--base-path", default="")
    ap.add_argument("--password", default=os.environ.get("DOOR_WEB_PASSWORD", ""))
    ap.add_argument("--auth-file", default=os.environ.get("DOOR_AUTH_FILE", ""))
    ap.add_argument("--board-log", action="store_true",
                    help="把板子每一条上行原样写进日志, 用于排查协议问题")
    ap.add_argument("--set-password", action="store_true")
    a = ap.parse_args(argv)

    store = Store(a.db)
    auth_file = a.auth_file or os.path.join(
        os.path.dirname(os.path.abspath(a.db)), "doorcenter-auth.json")
    auth = Auth(auth_file)

    if a.set_password:
        if not a.password:
            ap.error("--set-password 需要同时给 --password")
        auth.set_password(a.password)
        stamp("后台密码已写入 %s" % auth_file)
        return 0

    if not auth.exists():
        init = a.password or ("door-" + os.urandom(3).hex())
        auth.set_password(init)
        stamp("首次启动, 已生成凭据文件 %s" % auth_file)
        stamp("初始后台密码: %s  (登录后请立即修改; 重启或重新部署都不会重置)" % init)
    else:
        stamp("沿用已有凭据文件 %s" % auth_file)

    reg = Registry()
    gw = Gateway(store, reg, getattr(a, "board_log", False))
    app = App(store, auth, reg, gw, a.ui, a.base_path.rstrip("/"))
    Handler.app = app

    BoardServer(a.board_host, a.board_port, gw, reg).start()
    httpd = ThreadingHTTPServer((a.http_host, a.http_port), Handler)
    httpd.daemon_threads = True
    stamp("http api on %s:%d base=%r ui=%s" %
          (a.http_host, a.http_port, app.base, a.ui))
    store.add_event("system", "INFO", detail="service start ver=%s" % VER)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        stamp("bye")
    return 0


if __name__ == "__main__":
    sys.exit(main())
