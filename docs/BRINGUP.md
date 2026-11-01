# BRINGUP — 板子到手后的联调手册

固件已编译通过但**从未见过真机**。这份手册按时间顺序列出:烧录 → 看日志 → 配对 →
验证 → 出问题怎么定位。**核心原则:每一步失败都先拿日志,不要瞎猜。**

## 0. 硬件

- Raspberry Pi Pico 2 W ×1
- Switch 2 Pro 手柄(电量 >30% 为佳)
- Windows PC(已装 DS4Windows 或 Steam)

## 1. 烧录

1. WSL 里 `./build.sh` 确认 `build/sw2bridge.uf2` 存在
2. 按住 BOOTSEL 插 Pico → Windows 弹出 `RPI-RP2` 盘
3. 从 `\\wsl.localhost\<distro>\home\smy\workspace\switch2pro-bridge\build\` 拖入 uf2(或在 WSL 里直接 `cp build/sw2bridge.uf2 /mnt/d/` 再拖)
4. 板子自动重启,LED 开始闪烁(=扫描中)

## 2. 看日志

- 设备管理器 → "端口(COM 和 LPT)" → 找 `USB 串行设备(COMx)`
- PuTTY / Windows Terminal + `serial` 模式连接(任意波特率,CDC 不校验)
- 固件上电横幅:`sw2bridge v0.5 ...`

### LED 含义

| LED | 状态 |
|---|---|
| 慢闪(2s 周期) | 断线重试等待 |
| 中速闪(500ms) | 扫描/连接中 |
| 快闪(100ms) | 初始化/切换订阅 |
| **常亮** | **流式传输中(成功)** |

### 人格切换

**按住 BOOTSEL ≥1.5 秒**在 `switch-pro`(默认)与 `ds4` 人格间切换,USB 会重新枚举,
日志会打 `[usb] persona -> ...`。默认 Switch Pro 人格下 Windows 识别为
“Pro Controller”,Steam 需开启 设置→控制器→通用→**Switch Pro 配置支持**。

## 3. 连接手柄

按住手柄顶部**同步键**约 3 秒,直到玩家指示灯快速巡回闪烁(=BLE 可发现)。
固件会自动连上。**手柄需先脱离 Switch 2 主机**(主机睡眠即可,手柄进入配对模式时
会主动广播)。

## 4. 预期日志序列(成功路径)

```
sw2bridge v1.6 (Switch 2 Pro -> switch-pro persona, Pico 2 W)
[cfg] flash blank/invalid magic -> defaults
[sw2] ATT client ready (l2cap+gatt, local MTU 512)
[sw2] power on requested
[sw2] btstack up
[sw2] scanning...
[sw2] adv match pid=2069 rssi=-52 type=0 addr=XX:XX:...
[sw2] connecting to XX:XX:... (pid 2069)
[sw2] connected handle 0x00XX
[sw2] ATT MTU exchange in 150 ms
[sw2] ATT MTU -> 512                 ← 必须 >66,否则 63 字节报告会被截断
[sw2] discovery probe: primary services
[sw2] svc 1800 ... / svc 1801 ... / svc 128b ... <- Nintendo controller
[sw2] discovery complete: N service(s) — ATT alive
[sw2] enabling ACK CCC (0x001b)
[sw2] ACK CCC on; starting init sequence
[sw2] step 1/18 init-enable (03/0d len 8)
[sw2] cmd reply 03 01 01 0d ...
[sw2] step 5/18 feat-set-preamble (0c/02 len 4)     ← 掩码是 0x94,不是 0x2f
[sw2] step 9/18 select-report (03/0a len 4)
[sw2] step 16/18 ble-raw-selector (raw len 11)
[sw2] step 18/18 feat-enable (0c/04 len 4)          ← 掩码是 0x27
[sw2] init complete; enabling native input CCC
[sw2] native CCC on — streaming (native 0x09)
[sw2] first input report on 0x000e (native 0x09)    ← 或 0x000a (common 0x05 fallback)
[sw2] rx=1 btn=00000000 lx=2048 ly=2048 rx=2048 ry=2048
```

两个关键判读点:

1. **`first input report on ...`** —— 只要出现这一行,链路就是通的。出现
   `0x000a (common 0x05 fallback)` 说明手柄在 BLE 上走的是公共报告,固件会照常
   解码(功能不受影响,只是原生 0x09 独有的键要等 v2)。
2. **`step N/18` 卡住** —— 记下卡在第几步 + 最后一条 `cmd reply` 的 hexdump,这是
   定位 BLE 协议差异的唯一有效信息。

然后:Windows 设备管理器出现 `Pro Controller`(默认人格),Steam / joy.cpl 应识别。

**默认(Switch Pro)人格验证**:Steam 应显示为 Switch Pro 控制器,游戏内提示应是与
手柄丝印一致的 ABXY;按键是 1:1 直通(手柄 A=游戏 A,无需对换)。
**DS4 人格验证**:识别为 DS4,键位为 Xbox 布局(A/B、X/Y 对换)。

注意:Pro 人格下手柄的 C 键暂无映射(Switch 1 协议没有这个键);主机发来的震动
命令会打 `[pro] out report` 日志,翻译在 v2。

## 5. 故障矩阵

| 症状(日志) | 含义 | 处置 |
|---|---|---|
| 一直 `scanning...`,无 adv match | 手柄没进配对模式 / 广播格式与 Joy-Con 不同 | 长按同步键;若仍无,开启 `SW2_SCAN_DEBUG` 已默认打印所有 0553 厂商数据广播,把原始 hexdump 带回来 |
| `adv match pid=xxxx`(非 2069) | 连到了 Joy-Con 2 等其他手柄 | 正常现象(ANY_NS2 开着),或关掉宏只收 Pro |
| `connect failed status=xx` | 控制器拒绝连接 | 记录 status 码;间隔几秒会自动重试 |
| `connected` 后 2s 左右 `disconnected` | v0.3 根因:固件没调 `l2cap_init()`/`gatt_client_init()`,ATT 请求从未真正发出,手柄等到超时后断链 | v0.4 已修复;若仍出现,看日志是否停在 `requesting ATT MTU exchange` |
| `FATAL: MTU exchange unanswered` | 控制器没回 Exchange MTU Response。v0.4 以前的根因是请求值 1017 超 ATT 上限;v0.5 固定请求 512 | 确认启动日志有 `local MTU 512`;若仍出现,需要 HCI dump 看请求是否真的发到空口 |
| `FATAL: ATT setup stalled` / `CCC write rejected` | MTU 通了,但服务发现或 CCC 写入 2s 内没完成 | 记录最后一条 `svc`/`gatt query status`;检查句柄布局 |
| `FATAL: CCC write failed` 或 `gatt query status=xx`(xx 为 0x01~0x1e 标准码) | CCC 写入被拒:句柄不存在/不可写 → Pro 的 GATT 布局偏移 | 记录标准码:01=句柄无效,02/03=不可读写;需改 UUID 发现代替固定句柄 |
| `gatt query status=7f` | btstack `ATT_ERROR_TIMEOUT`(30s):请求排队后从未真正发出/无人应答。v0.3 上是 `gatt_client_init()` 缺失,`CAN_SEND_NOW` 永远不派发到 GATT client | v0.4 不应再出现;若再现,把日志带回 |
| `ATT MTU -> 23` 或无此行、报告 len<63 | MTU 交换没发生/被拒 | v0.5 正常应打印 `ATT MTU -> 512`;若为 23,查 `requesting ATT MTU exchange` 之后的日志 |
| 某一步 `command reply timeout` | 该步命令没有被 ACK | 记录卡在第几步 + 最后一条 reply hexdump。v1.5 序列已对齐 `linux-switch2/switch2-init.c`;若仍在同一步失败,说明 Pro 的 BLE 侧与该驱动(Joy-Con 实测)确有差异 |
| `step N/18 ... skipped (X B > MTU 23-3)` | MTU 没协商成功,长命令被跳过 | 先看有没有 `ATT MTU -> 512`;没有就是 MTU 交换没成功 |
| `gatt query status=02/03` | 写句柄不存在(Pro 的 GATT 布局偏移) | 需要做一次 UUID 发现代替固定句柄,把日志带回来 |
| `streaming but no native report yet` | 原生输入 CCC 已开,但 2s 内没有 0x09 通知 | v1.5 起已同时订阅 0x000a 公共报告,若两路都没有:检查 `select-report` 那一步是否真的被 ACK |
| Steam 不识别为 Pro / 提示不是 ABXY | 描述符或报告长度问题 | 日志确认人格为 switch-pro;0x30 报文已按真机/描述符补零到 64 字节发送,若 SDL 仍读取异常把现象与日志带回来 |
| 按键全错/摇杆偏 | 布局或校准差异 | 对照日志里的 serial/校准 hexdump 和 Valve 映射表修正 |

## 6. 如果需要抓包(REA 路径)

协议缺口用抓包补:

1. **HCI 层**:给固件加 `WANT_HCI_DUMP`(btstack_cyw43 自带,hci_dump 走 stdout/CDC),
   或在 Linux 主机上用 `btmon` 复现(需要 hid-switch2 DKMS + bluez-switch2)。
2. **空口层**:若手头有第二块 Pico 2 W / nRF Sniffer / ESP32,抓配对过程 BLE 广播与
   GATT 流量存 pcap。
3. 把 pcap/日志交给会话,用 REA 的 `inspect_packet_capture`/`inspect_pcap_capture`
   逐步对齐 PROTOCOL.md。

## 7. Windows 侧验证清单

- [ ] joy.cpl(运行 `joy.cpl`)出现 controller,按键/摇杆/扳机有响应,键位为 Xbox 布局
- [ ] DS4Windows:识别为 DS4,可映射为 XInput(游戏内震动请求会打日志,当前忽略)
- [ ] Steam:Big Picture 识别 DS4( gyro 透传是 v2,当前为占位数据)
- [ ] 拔插 / 手柄关机再开:1 秒内自动重连(LED 回到常亮)
