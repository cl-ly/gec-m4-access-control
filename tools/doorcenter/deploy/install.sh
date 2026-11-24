#!/bin/sh
# 门禁中控一键部署 / 升级脚本 (Ubuntu 24.04, 纯标准库, 不需要 pip).
#
# 用法 (在服务器上, 包已解到当前目录):
#   sudo sh install.sh
#
# 幂等: 重复执行只更新代码并重启服务, 绝不重置后台密码.
set -eu

APP_DIR=/opt/doorcenter
DATA_DIR=/var/lib/doorcenter
AUTH_FILE=$DATA_DIR/doorcenter-auth.json
SVC_USER=doorcenter
DEPLOY_DIR=$(cd "$(dirname "$0")" && pwd)
SRC_DIR="$DEPLOY_DIR"
# 安装包解开后 deploy/ 与 doorcenter.py 同级, 本脚本自己在 deploy/ 里,
# 所以找不到 doorcenter.py 就往上一级取源码根.
if [ ! -f "$SRC_DIR/doorcenter.py" ]; then
  SRC_DIR=$(cd "$SRC_DIR/.." && pwd)
fi

if [ "$(id -u)" -ne 0 ]; then
  echo "需要 root: sudo sh install.sh" >&2
  exit 1
fi

# ---- 0. 先停旧实例 ----
# 必须在端口探测之前: 否则上一版 doorcenter 还占着自己的端口,
# 会被误判成"被他人占用", 于是每次重装端口都往后顺延一位。
systemctl stop doorcenter 2>/dev/null || true

PY=/usr/bin/python3
[ -x "$PY" ] || PY=$(command -v python3)
echo "== python: $($PY -V 2>&1) ($PY)"

# ---- 1. 用户与目录 ----
if ! id "$SVC_USER" >/dev/null 2>&1; then
  useradd --system --home "$APP_DIR" --shell /usr/sbin/nologin "$SVC_USER"
  echo "== 已创建服务账号 $SVC_USER"
fi
install -d -m 755 "$APP_DIR" "$APP_DIR/ui"
install -d -m 750 -o "$SVC_USER" -g "$SVC_USER" "$DATA_DIR"

# ---- 2. 代码 ----
install -m 644 "$SRC_DIR/doorcenter.py" "$APP_DIR/doorcenter.py"
for f in "$SRC_DIR"/ui/*; do
  [ -f "$f" ] && install -m 644 "$f" "$APP_DIR/ui/$(basename "$f")"
done
echo "== 代码已更新到 $APP_DIR"

# ---- 3. 后台密码: 只在第一次部署时生成 ----
FIRST_RUN=0
if [ ! -f "$AUTH_FILE" ]; then
  FIRST_RUN=1
  # 密码不再写进数据库, 而是独立凭据文件; 之后升级、重装都不碰它.
  PW=$(head -c 9 /dev/urandom | base64 | tr -dc 'A-Za-z0-9' | head -c 12)
  sudo -u "$SVC_USER" "$PY" "$APP_DIR/doorcenter.py" \
      --db "$DATA_DIR/door.db" --auth-file "$AUTH_FILE" \
      --password "$PW" --set-password >/dev/null
  echo "$PW" > "$DATA_DIR/initial-password.txt"
  chown "$SVC_USER:$SVC_USER" "$DATA_DIR/initial-password.txt"
  chmod 600 "$DATA_DIR/initial-password.txt"
  echo "== 首次部署, 初始后台密码: $PW"
  echo "   (已另存一份到 $DATA_DIR/initial-password.txt, 登录后请立即改密)"
else
  echo "== 凭据文件已存在, 保留用户自定义的密码, 不做任何重置"
fi
chown -R "$SVC_USER:$SVC_USER" "$APP_DIR"
chown -R "$SVC_USER:$SVC_USER" "$DATA_DIR"

# ---- 4. 清掉旧的手工进程, 避免抢 8080 ----
pkill -f '/tmp/door.py' 2>/dev/null || true
if command -v ss >/dev/null 2>&1; then
  HOLD=$(ss -ltnp 2>/dev/null | grep ':8080 ' | grep -v doorcenter || true)
  if [ -n "$HOLD" ]; then
    echo "!! 8080 仍被占用: $HOLD"
  fi
fi

# ---- 5. systemd ----
# 后台端口默认 8081; 一旦被无关进程长期占用, 就选一个空闲端口并把结果写进
# $DATA_DIR/http-port 持久复用, 避免每次重装都换端口 (前端与反代都按它生成).
port_free() {
  if command -v ss >/dev/null 2>&1; then
    ! ss -ltn 2>/dev/null | grep -q ":$1 "
  else
    ! (exec 3<>/dev/tcp/127.0.0.1/"$1") 2>/dev/null
  fi
}
PORT_FILE="$DATA_DIR/http-port"
HTTP_PORT=""
if [ -f "$PORT_FILE" ]; then
  HTTP_PORT=$(tr -dc '0-9' < "$PORT_FILE")
fi
if [ -z "$HTTP_PORT" ] || ! port_free "$HTTP_PORT"; then
  HTTP_PORT=""
  for p in 8081 8082 8083 8084 8085 8086 8087; do
    if port_free "$p"; then HTTP_PORT=$p; break; fi
  done
fi
if [ -z "$HTTP_PORT" ]; then
  echo "!! 8081-8087 全被占用, 无法选后台端口" >&2
  exit 1
fi
echo "$HTTP_PORT" > "$PORT_FILE"
chown "$SVC_USER:$SVC_USER" "$PORT_FILE"
echo "== 后台 HTTP 端口 $HTTP_PORT (记录在 $PORT_FILE)"
sed "s/--http-port 8081/--http-port $HTTP_PORT/" \
    "$DEPLOY_DIR/doorcenter.service" > /etc/systemd/system/doorcenter.service
chmod 644 /etc/systemd/system/doorcenter.service
systemctl daemon-reload
systemctl enable doorcenter >/dev/null
systemctl restart doorcenter
sleep 2
systemctl is-active doorcenter >/dev/null || {
  echo "!! 服务没起来, 最近日志:" >&2
  journalctl -u doorcenter -n 40 --no-pager >&2 || true
  exit 1
}
echo "== doorcenter 已设为开机自启并运行中"

# ---- 6. nginx 反代 ----
if command -v nginx >/dev/null 2>&1; then
  if [ -f /etc/nginx/sites-available/default ] && [ ! -f /etc/nginx/sites-available/default.bak-doorcenter ]; then
    cp -a /etc/nginx/sites-available/default /etc/nginx/sites-available/default.bak-doorcenter
    echo "== 已备份原 nginx 站点配置"
  fi
  sed "s#127.0.0.1:8081#127.0.0.1:$HTTP_PORT#g" \
      "$DEPLOY_DIR/nginx-doorcenter.conf" > /tmp/dc-nginx.conf
  install -m 644 /tmp/dc-nginx.conf /etc/nginx/sites-available/default
  if nginx -t 2>/dev/null; then
    systemctl reload nginx
    echo "== nginx 已加载 /door/ 反代"
  else
    echo "!! nginx 配置校验失败, 已回滚" >&2
    if [ -f /etc/nginx/sites-available/default.bak-doorcenter ]; then
      cp -a /etc/nginx/sites-available/default.bak-doorcenter /etc/nginx/sites-available/default
      nginx -t 2>/dev/null && systemctl reload nginx
    fi
    exit 1
  fi
else
  echo "== 未安装 nginx, 跳过反代 (可直接访问 8081)"
fi

# ---- 7. 自检 ----
echo "== 自检"
sleep 1
curl -fsS --max-time 5 "http://127.0.0.1:$HTTP_PORT/api/health" && echo || echo '!! 本地 health 检查失败'
IP=$(hostname -I 2>/dev/null | awk '{print $1}')
echo "== 后台地址: http://${IP:-<服务器IP>}/door/"
echo "== 板子网关: ${IP:-<服务器IP>}:8080 (与原 v4 协议一致, 固件无需改动)"
[ "$FIRST_RUN" = "1" ] && echo "== 初始密码见上方输出"
echo "== 完成"
