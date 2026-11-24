#!/bin/sh
# Build, upload and install doorcenter on the door server.
#
# Works from Windows git-bash too: Windows native scp cannot take MSYS
# /d/... paths, so we always cd into the package dir and upload a bare
# file name.
#
# Usage:  sh deploy/release.sh [--open-test]
set -u

# 服务器地址不进仓库, 一律由环境变量提供: DOOR_HOST=user@host sh deploy/release.sh
HOST="${DOOR_HOST:?请先设置 DOOR_HOST, 例如 DOOR_HOST=user@your-server}"
KEY="${DOOR_KEY:-$HOME/.ssh/id_ed25519}"
SRC=$(cd "$(dirname "$0")/.." && pwd)
OUT="${DOOR_OUT:-/d/deepseek harness/.caches/tmp/dc-release}"
TARBALL=doorcenter-full.tgz
OPTS="-o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=12 -i $KEY"
OPEN_TEST=0
[ "${1:-}" = "--open-test" ] && OPEN_TEST=1

step() { echo; echo "== $*"; }

step package
cd "$SRC" || exit 1
mkdir -p "$OUT"
chmod +x deploy/install.sh
rm -f "$OUT/$TARBALL"
tar czf "$OUT/$TARBALL" doorcenter.py ui deploy selftest.py
sha256sum "$OUT/$TARBALL" 2>/dev/null || true

step upload
cd "$OUT" || exit 1
scp $OPTS "./$TARBALL" "$HOST:/tmp/$TARBALL" 2>&1 | tail -2

step install
ssh $OPTS "$HOST" "rm -rf /tmp/dc; mkdir -p /tmp/dc; tar xzf /tmp/$TARBALL -C /tmp/dc; sudo -n sh /tmp/dc/deploy/install.sh 2>&1 | tail -18" 2>&1 | tail -22

step verify
ssh $OPTS "$HOST" 'bash -s' <<'REMOTE'
PORT=$(sudo -n cat /var/lib/doorcenter/http-port 2>/dev/null | tr -dc '0-9')
[ -n "$PORT" ] || PORT=8081
echo "http_port=$PORT"
systemctl is-enabled doorcenter
systemctl is-active doorcenter
curl -fsS --max-time 5 "http://127.0.0.1:$PORT/api/health" || echo HEALTH_FAIL
echo
curl -sS -o /dev/null -w "web_page=%{http_code}\n" --max-time 5 "http://127.0.0.1:$PORT/"
ss -ltnp 2>/dev/null | grep -E ":8080 |:$PORT " || true
REMOTE

if [ "$OPEN_TEST" = "1" ]; then
  step open-test
  ssh $OPTS "$HOST" 'bash -s' <<'REMOTE'
PORT=$(sudo -n cat /var/lib/doorcenter/http-port 2>/dev/null | tr -dc '0-9'); [ -n "$PORT" ] || PORT=8081
PW=$(sudo -n cat /var/lib/doorcenter/initial-password.txt 2>/dev/null | tr -d '\r\n')
[ -n "$PW" ] || { echo NO_PASSWORD_FILE; exit 0; }
J=/tmp/dc.jar; rm -f "$J"
curl -sS -c "$J" -X POST -H 'Content-Type: application/json' \
  -d "{\"password\":\"$PW\"}" "http://127.0.0.1:$PORT/api/login" >/dev/null || echo LOGIN_FAIL
curl -sS -b "$J" -X POST "http://127.0.0.1:$PORT/api/open"; echo
sleep 8
sudo -n journalctl -u doorcenter --since '-20s' --no-pager 2>/dev/null \
  | grep -E 'board line|sent |remote open|ack' | tail -12
curl -sS -b "$J" "http://127.0.0.1:$PORT/api/events?limit=4" \
  | python3 -c 'import json,sys
for e in json.load(sys.stdin)["events"]: print(e["ts"], e["kind"], e["result"], e["latency_ms"])'
REMOTE
fi

echo
echo "== release done"
