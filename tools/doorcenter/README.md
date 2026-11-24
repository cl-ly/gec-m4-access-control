# 门禁中控 doorcenter

门禁工程的服务器端：一个纯标准库 Python 进程，同时承担板子网关、Web 后台与记录落库。
固件侧不改一行、不重烧。

## 它做什么

| 端口 | 职责 |
|---|---|
| 8080 | 板子（ESP8266）TCP 长连接网关。与旧 v4 服务端逐字节兼容：`PING`→`PONG`、`CARD,<UID>,OK|DENY`、`PWD,OK|DENY|LOCK`、下行 `OPEN`→板子回 `OK` |
| 8081（仅本机） | HTTP 后台 API 与静态页面，由 nginx 用 `/door/` 反代到公网 |
| SQLite | 所有事件带 Asia/Shanghai 时间戳，含开锁耗时与来源 IP |

## 文件

```
doorcenter.py            服务端主体（网关 + API + 落库）
ui/index.html|app.css|app.js   手机可用的后台界面
selftest.py              本机全链路自测（假板子冒充 ESP8266 + 走一遍 HTTP API）
deploy/install.sh        幂等安装/升级脚本
deploy/doorcenter.service   systemd 单元
deploy/nginx-doorcenter.conf   nginx 反代站点
deploy/cdp_browser.py    浏览器级 DevTools 客户端（headless 下驱动页面的应急通道）
```

## 部署与升级

```sh
# 在服务器上（包已解开）
sudo sh deploy/install.sh

# 或在本机一键发布（打包 + 上传 + 安装 + 自检，--open-test 追加一次真实开门）
sh deploy/release.sh --open-test
```

脚本幂等：建服务账号与目录、装 systemd 开机自启、配置 nginx `/door/` 反代（校验失败自动回滚）、清掉抢 8080 的旧 `/tmp/door.py`。重复执行只更新代码并重启服务。

数据都在 `/var/lib/doorcenter/`：`door.db`（事件与门卡）、`doorcenter-auth.json`（后台密码摘要）、`initial-password.txt`（首次部署生成的初始密码）、`http-port`（实际端口，持久复用）。

## 后台密码为什么不在数据库里

密码存独立凭据文件 `doorcenter-auth.json`（pbkdf2-hmac-sha256，20 万轮，文件 600 权限），只存摘要与盐。
于是清库、换库、重装、重启都**不会**把密码打回初始值；只有显式执行：

```sh
sudo -u doorcenter python3 /opt/doorcenter/doorcenter.py \
  --db /var/lib/doorcenter/door.db \
  --auth-file /var/lib/doorcenter/doorcenter-auth.json \
  --password '新密码' --set-password
```

## 远程开门怎么保证不丢

双路径投递，均在服务端完成：

1. 立即向存活连接下发一次 `OPEN\n`；
2. 同时在「活跃连接」（最近 25 s 内有数据）上记一笔待补发，板子下一次 `PING` 时用 `PONG\nOPEN\n` 同一批字节捎带。

原因：固件 `Esp_LinkSend` 在等 `SEND OK` 的窗口内会吞掉串口环形缓冲字节，服务端若撞上板子发 `PING` 的那一刻，单路径的 `OPEN` 就会被丢弃。收到板子 `OK` 后立即清掉待补发，避免开两次门。

## 自测

```sh
python3 selftest.py
```

在临时端口起服务、用假板子跑完整协议、按 UI 的方式调用 HTTP API，并校验重启后密码仍为用户自定义值。共 30 项断言。

## 排查

```sh
sudo journalctl -u doorcenter -n 50 --no-pager
```

服务带 `--board-log`，会把板子每一条上行原样写进日志（`board line <ip> <- 'PING'`），用于确认协议是否真的到达。
