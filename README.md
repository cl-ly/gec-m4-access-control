# GEC-M4 智能门禁系统

基于粤嵌 GEC-M4 黄色开发板和 STM32F407ZET6 的嵌入式门禁示例工程。系统集成密码开门、RC522 刷卡、4x4 薄膜键盘、SSD1306 OLED，以及 HC-SR04 超声波靠近检测和低功耗门控。

当前仓库发布版本为 `alpha`，固件源代码位于 `AccessControl/`。

## 功能

- 4x4 薄膜键盘输入密码，支持数字 `0-9` 和字母 `A-D`。
- 支持数字与字母组合密码，例如 `1A2B`。
- `*` 为清除/取消功能键，`#` 为确认功能键；特殊符号不会写入密码。
- 空输入按 `#` 进入改密流程。
- RC522 刷卡开门，首次刷卡可自动配卡。
- SSD1306 128x64 软件 SPI OLED 显示。
- 上电后播放低音量提示音。
- HC-SR04 检测距离小于 `0.5 m` 时点亮 OLED。
- 连续约 `1.2 s` 距离大于 `0.5 m` 后关闭 OLED，并执行 `WFI` 睡眠等待。
- 任意键可唤醒屏幕，并保持一段时间方便操作。
- 距离页显示实时距离和 Echo 诊断信息。

## 目录

```text
AccessControl/
  Core/                 应用代码和外设驱动
  Drivers/              STM32F4 HAL 和 CMSIS
  MDK-ARM/              Keil MDK 工程
  tools/                OLED 字库相关工具
```

## 硬件接线

| 模块 | 接线 | 说明 |
|---|---|---|
| HC-SR04 Trig | `PE12`，开发板丝印 `D9` | 触发输出 |
| HC-SR04 Echo | `PE14`，开发板丝印 `D11` | 回波输入 |
| HC-SR04 VCC | `5V` | 必须供电 |
| HC-SR04 GND | `GND` | 与开发板共地 |
| OLED SCK | `PD7` | 软件 SPI 时钟 |
| OLED SDA | `PC6` | 软件 SPI 数据 |
| OLED CS | `PD6` | 片选 |
| OLED DC | `PB7` | 数据/命令选择 |
| OLED RES | `PA4` | 复位 |
| OLED VCC | `3.3V` | 不接 5V |
| RC522 SCK | `PG12` | 软件 SPI |
| RC522 MISO | `PD5` | 输入上拉 |
| RC522 MOSI | `PD4` | 软件 SPI |
| RC522 RST | `PE10` | 复位 |
| RC522 CS | `PF12` | 片选 |

4x4 薄膜键盘使用以下 8 根线：

```text
PG15 / PC7 / PC9 / PG9 / PB6 / PE6 / PA8 / PC8
```

键盘不是按常见的“前四根行线、后四根列线”排列。当前实测矩阵为：

```text
1/2/3/A：{0,6} {0,5} {0,7} {0,3}
4/5/6/B：{1,6} {1,5} {1,7} {1,3}
7/8/9/C：{2,6} {2,5} {2,7} {2,3}
*/0/#/D：{4,6} {4,5} {4,7} {4,3}
```

## 构建

工程使用 Keil MDK uVision 5.14 和 ARMCC5。打开以下工程文件：

```text
AccessControl/MDK-ARM/AccessControl.uvprojx
```

Keil 命令行全量构建时，将以下占位路径替换成本机的 uVision 和仓库路径：

```powershell
$uv4 = "<Keil MDK 安装目录>\UV4\UV4.exe"
$project = "<仓库目录>\AccessControl\MDK-ARM\AccessControl.uvprojx"
Start-Process -FilePath $uv4 -ArgumentList ('-r "' + $project + '" -j0') -Wait -PassThru
```

构建产物位于：

```text
AccessControl/MDK-ARM/Objects/AccessControl.hex
```

当前 alpha 版本已在本机实测构建通过，结果为 `0 Error(s), 0 Warning(s)`。

## 烧录

使用 J-Link 的 SWD 接口烧录，不能使用 JTAG。烧录固件时不要无条件执行全片擦除，因为门卡、密码和键盘表分别保存在 Flash 扇区 5、6 和 7 尾部。

推荐使用只加载 HEX 覆盖程序区的方式：

```text
loadfile AccessControl.hex
r
g
qc
```

J-Link 返回的校验结果以 `O.K.` 为准。烧录后建议进入距离页，检查距离值和底部 `W/H` 诊断行。

## 持久化数据

| 内容 | 地址/区域 | 说明 |
|---|---|---|
| 门卡 UID | `0x08020000`，扇区 5 | 首卡自动配卡 |
| 密码 | `0x08040000`，扇区 6 | 默认密码为 `1234` |
| 键盘线对表 | `0x0807C000`，扇区 7 尾部 | 当前固件使用内置实测表 |

## 当前验证状态

- HC-SR04 实测回波已经进入 `PE14`，距离测量正常。
- 距离约 `5 cm` 时屏幕保持点亮。
- 距离约 `67 cm` 持续超过门控时间后，`g_disp_on=0`，程序进入 `HAL_PWR_EnterSLEEPMode`。
- 数字和 `A-D` 字母可以组合输入密码。
- `*`、`#` 保持功能键语义，不作为密码字符。

仓库内容仅包含门禁固件、构建依赖、第三方许可证和本代码说明。

## 版本

当前发布：`alpha`

GitHub 仓库：<https://github.com/cl-ly/gec-m4-access-control>
