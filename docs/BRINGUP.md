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
- 固件上电横幅:`sw2bridge v0.2 ...`

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
sw2bridge v0.3 ...
[sw2] btstack up
[sw2] scanning...
[sw2] adv match pid=2069 rssi=-52 type=0 addr=XX:XX:...
[sw2] connecting to XX:XX:... (pid 2069)
[sw2] connected handle 0x00XX
[sw2] enabling cmd-reply CCC
[sw2] ATT MTU -> 247            ← 必须 >66,否则报告会被截断!
[sw2] cmd-reply CCC on; enabling native CCC
[sw2] CCCs set; starting init sequence
[sw2] step 1/21 init-enable (03/0d len 8)
[sw2] cmd reply 03 01 01 0d ...
... (step 2~10)
[sw2] first native report seen (+10ms sync)
... (step 11~21, 中间有 serial/校准 hexdump 与 fw 版本行)
[sw2] init complete; unsubscribing native
[sw2] subscribing common 0x05
[sw2] common report flowing — streaming (common)    ← LED 常亮
```

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
| `connected` 后立刻 `disconnected` | 手柄主动断链(曾因 master 发 L2CAP 参数更新请求违规,已移除) | 若复现频繁,把日志带回来 |
| `FATAL: ATT silent after connect (MTU exchange stalled?)` | 连接后 2s 内 ATT 无任何进展:手柄不应答 MTU 交换,后续写入全部被阻塞 | 每次快钔2s 重试;若持续出现,需改用「无 MTU 交换」模式联调(把日志带回来) |
| `FATAL: CCC write failed` 或 `gatt query status=xx`(xx 为 0x01~0x1e 标准码) | CCC 写入被拒:句柄不存在/不可写 → Pro 的 GATT 布局偏移 | 记录标准码:01=句柄无效,02/03=不可读写;需改 UUID 发现代替固定句柄 |
| `gatt query status=7f` | btstack `ATT_ERROR_TIMEOUT`:请求根本没被应答(常见于 MTU 交换挂起把队列堵死) | 修复后不应再出现;若再现说明手柄又进入不应答状态,对照上面 ATT silent 行 |
| `ATT MTU -> 23` 或无此行、报告 len<63 | MTU 交换没发生 | 报告会截断,把日志带回来(需要在首个 GATT 操作前显式触发 MTU 交换) |
| 某一步 `command reply timeout` | Pro 的初始化序列与 Joy-Con 有差异 | **最关键的未知数**:记录卡在第几步 + 最后一条 reply hexdump |
| `gatt query status=02/03` | 写句柄不存在(Pro 的 GATT 布局偏移) | 需要做一次 UUID 发现代替固定句柄,把日志带回来 |
| 走到 `common 0x05 silent — falling back to native` | 公共报告订阅后无数据 | 固件自动回退原生 0x09 解析;若原生也没数据,需补 §38 的 `05` 选择命令 |
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
