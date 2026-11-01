# 协议速查(Switch 2 手柄 · BLE / USB)

> 这是从上游参考实现里摘出来的**事实摘要**,不是原文拷贝;实现以 `firmware/` 为准,
> 数字以 `refs/` 原文为准。两者分歧时,先看 `refs/linux-switch2/PROTOCOL.md`,
> 再同时改代码和本文。本文只收录本固件真正依赖的部分。

## 0. 来源

| 用途 | 仓库 | 许可 |
|---|---|---|
| 协议权威来源;内核驱动(含原生按键位、震动编码) | [novakpetya/linux-switch2](https://github.com/novakpetya/linux-switch2) | GPL-2.0-or-later |
| 实测的固定 GATT 句柄、BLE 命令节奏 | [novakpetya/bluez-switch2](https://github.com/novakpetya/bluez-switch2) | GPL-2.0-or-later |
| Windows 侧驱动行为(= 我们的仿真目标) | [libsdl-org/SDL](https://github.com/libsdl-org/SDL) `SDL_hidapi_switch*.c` | zlib |

`refs/` 不入库(体积大)。换机器后重新拉:

```bash
mkdir -p refs && cd refs
git clone --depth 1 https://github.com/novakpetya/linux-switch2
git clone --depth 1 https://github.com/novakpetya/bluez-switch2
```

## 1. 身份

| 手柄 | VID:PID | 原生报告 | 公共报告 | 输出报告 |
|---|---|---|---|---|
| Joy-Con 2 左 | `057e:2067` | `0x07` | `0x05` | `0x01` |
| Joy-Con 2 右 | `057e:2066` | `0x08` | `0x05` | `0x01` |
| Switch 2 Pro | `057e:2069` | `0x09` | `0x05` | `0x02` |
| NSO GameCube | `057e:2073` | `0x0a` | `0x05` | `0x03` |

固件模拟的是 **`057e:2009`(Switch 1 Pro)**,不是上表里的 `0x2069` —— 原因见 §9。

固件信息里返回的控制器类型:`0`=JC2 左、`1`=JC2 右、`2`=Pro、`3`=GC。

## 2. BLE 广播与连接

- 厂商数据 company `0x0553`,载荷 `01 00 03 7e 05 <PID LE16>`
- 连接参数:interval 6/6、latency 0、supervision 42(= 7.5 ms / 420 ms)
- **不需要 SMP 配对**;控制器收到配对请求会回 "Pairing Not Supported" 并断链

## 3. GATT 固定句柄(bluez-switch2 实测)

| 句柄 | 含义 |
|---:|---|
| `0x0001`–`0x0007` | bootstrap 服务 |
| `0x0008`–`0x002a` | Nintendo 控制器服务(`ab7de9be-89fe-49ad-828f-118f09df7fd0`) |
| `0x000a` / `0x000b` | 公共输入值(`...7fd2`)/ 其 CCC |
| `0x000e` / `0x000f` | 侧/原生输入值 / 其 CCC |
| `0x0012` | 震动(WRITE WITHOUT RESPONSE) |
| `0x0014` | 命令(`649d4ac9-8eb7-4e6c-af44-1ea54fe5f005`) |
| `0x001a` / `0x001b` | 命令回复(`c765a961-...-b111836a`)/ 其 CCC |
| `0x002b`–`0x0030` | GAP / GATT |

固件按**固定句柄**访问,不做 UUID 发现;换固件版本导致句柄偏移时会写失败(日志里能看到)。

## 4. 命令帧

8 字节头:

| 偏移 | 大小 | 含义 |
|---:|---:|---|
| 0 | 1 | 命令 ID |
| 1 | 1 | 方向/状态,主机请求=`0x91` |
| 2 | 1 | 传输:`0x00` USB / `0x01` Bluetooth |
| 3 | 1 | 子命令 |
| 4 | 1 | 未知/状态 |
| 5 | 1 | 载荷长度 |
| 6–7 | 2 | 保留(0) |

回复头以 `[命令, 状态, transport, 子命令, ...]` 开头,状态字节 BLE 常见 `0x78`、USB `0xf8`。
命令载荷上限 56 字节。固件以**写无响应**方式发到 `0x0014`,回复走 `0x001a` 通知。

## 5. BLE 初始化顺序(18 步)

顺序取自 `linux-switch2/switch2-init.c`,固件 `firmware/sw2_ble.c` 的 `init_acts[]` 一一对应。

| # | 代码里的名字 | 命令 | 子命令 | 载荷要点 |
|---:|---|---|---|---|
| 1 | init-enable | `0x03` | `0x0d` | `01 00 ff ff ff ff ff ff` |
| 2 | ble-setup1 | `0x07` | `0x01` | — |
| 3 | ble-setup2 | `0x16` | `0x01` | — |
| 4 | ble-pair | `0x15` | `0x03` | `00` |
| 5 | feat-set-preamble | `0x0c` | `0x02` | `94 00 00 00` |
| 6 | ble-motion | `0x11` | `0x03` | — |
| 7 | vib-setup | `0x0a` | `0x08` | 20 字节扩展震动配置 |
| 8 | feat-enable-preamble | `0x0c` | `0x04` | `94 00 00 00` |
| 9 | select-report | `0x03` | `0x0a` | `09 00 00 00` |
| 10 | fw-info | `0x10` | `0x01` | — |
| 11 | nfc-off | `0x01` | `0x0c` | — |
| 12 | nfc-on | `0x01` | `0x01` | `00 00 00 00` |
| 13 | player-led | `0x09` | `0x07` | `01 00 00 00 00 00 00 00` |
| 14 | cal-L-factory | `0x02` | `0x04` | 读 `0x130a8`,9 字节 |
| 15 | cal-R-factory | `0x02` | `0x04` | 读 `0x130e8`,9 字节 |
| 16 | ble-raw-selector | — | — | 裸帧 `01 00×8 03 30`,**无回复** |
| 17 | feat-set | `0x0c` | `0x02` | `27 00 00 00` |
| 18 | feat-enable | `0x0c` | `0x04` | `27 00 00 00` |

两个易错点:

- **两个特性掩码不能混**:`0x94` 是 BLE 传输前导,`0x27` 才是普通控制器特性集。`0x08` 不是特性位。
- **读闪存的子命令有两说**:内核驱动用 `0x04`(本项目采用),SDL 的 USB 路径用 `0x01`。
  如果实机日志出现 `flash addr mismatch`,说明控制器不认 `0x04`,改成 `0x01` 再试。

读闪存请求的载荷是 8 字节:`<len> 7e 00 00 <addr LE32>`;回复载荷前 8 字节回显同样的
子头,数据从回复偏移 16 开始。

## 6. 输入报告

### 6.1 公共 `0x05`(GATT 偏移,不含报告号)

| 偏移 | 大小 | 字段 |
|---:|---:|---|
| `0x00` | 4 | 32 位计数器 |
| `0x04` | 4 | 按键 |
| `0x0a` / `0x0d` | 3 / 3 | 左/右摇杆(12 位打包) |
| `0x1f` | 2 | 电池电压 mV |
| `0x21` | 1 | 充电状态 |
| `0x2a` | 18 | 运动(accel + gyro) |
| `0x30` / `0x36` | 6 / 6 | accel / gyro,各 3×s16 |

### 6.2 公共按键位

| 字节 | 位 |
|---|---|
| 0 | 0 Y、1 X、2 B、3 A、4 SR(右)、5 SL(右)、6 R、7 ZR |
| 1 | 0 Minus、1 Plus、2 RS、3 LS、4 Home、5 Capture、6 C |
| 2 | 0 下、1 上、2 右、3 左、4 SR(左)、5 SL(左)、6 L、7 ZL |
| 3 | 0 GR、1 GL |

### 6.3 原生 Pro `0x09`(GATT 偏移)

| 偏移 | 字段 |
|---:|---|
| `0x00` | 计数器 |
| `0x01` | 电源字节 |
| `0x02` | 字节 0:0 B、1 A、2 Y、3 X、4 R、5 ZR、6 Plus、7 RS |
| `0x03` | 字节 1:0 下、1 右、2 左、3 上、4 L、5 ZL、6 Minus、7 LS |
| `0x04` | 字节 2:0 Home、1 Capture、2 **GR**、3 **GL**、4 C |
| `0x05` / `0x08` | 左/右摇杆(12 位打包) |
| `0x0b` 起 | 原生报告**不含 IMU** |

> 位序以 `switch2.h` 的 `NS2_BTN_PRO_*` 为准(`NS2_BTN_PRO_GR = BIT(2)`、
> `NS2_BTN_PRO_GL = BIT(3)`),并与独立实现 ns2pro 的 `BUTTON_MAP` 交叉验证一致。
> 原生 0x09 的布局在上游没有单独文档,固件按"USB HID 偏移减 1"推导,即
> `BUTTON_OFFSET=3`、摇杆 `6/9`(HID 坐标)。

### 6.4 电源字节

| 位 | 含义 |
|---|---|
| 0 | 外部供电 |
| 1 | 充电中 |
| 2–5 | 电量等级 0–9 |
| 6–7 | 保留 |

## 7. 闪存与校准

| 地址 | 大小 | 含义 |
|---:|---:|---|
| `0x13002` | `0x10` | 序列号/控制器信息 |
| `0x13040` | `0x10` | BLE 陀螺仪校准 |
| `0x130a8` / `0x130e8` | 9 / 9 | 工厂摇杆校准(左/右) |
| `0x1310c` | `0x0c` | BLE 加速度计校准 |
| `0x1fc040` / `0x1fc080` | 11 / 11 | 用户摇杆校准(左/右,前 2 字节 magic `a1b2`) |

9 字节校准块:**neutral X/Y、正向行程 X/Y、负向行程 X/Y**,每维 12 位打包:

```text
X = b0 | ((b1 & 0x0f) << 8)
Y = (b1 >> 4) | (b2 << 4)
```

全 `0xff` 表示没有校准数据。用户校准在同样结构前加 2 字节 magic。

> Switch 1 的 SPI 校准块格式不同(参数是"相对中心的正负行程"),固件在 `pro.c`
> 的 `pro_stick_cal_blob()` 里做转换,并用 `tests/run.sh` 对照 SDL 的解码器验证。

## 8. 震动

主机侧(SDL)发的是**经典 4 字节 HD 震动块**,每颗马达一份:

```text
b0 = hi_freq & 0xff
b1 = hi_amp | ((hi_freq >> 8) & 0x01)
b2 = lo_freq | ((lo_amp >> 8) & 0x80)
b3 = lo_amp & 0xff
```

控制器侧要的是 **5 字节 (freq, amp)×2 帧**:

```text
f0 = hi_freq & 0xff
f1 = ((hi_freq >> 8) & 0x03) | ((hi_amp & 0x3f) << 2)
f2 = ((hi_amp >> 6) & 0x0f) | ((lo_freq & 0x0f) << 4)
f3 = ((lo_freq >> 4) & 0x3f) | ((lo_amp & 0x03) << 6)
f4 = (lo_amp >> 2) & 0xff
```

USB 输出报告 `0x02` 的布局(Pro,两颗 LRA):

```text
[0]=0x02 [1]=0x50|seq [2..6]=帧 [0x11]=0x50|seq [0x12..0x16]=帧
```

BLE 上把同样的报告写到 `0x0012`;驱动按 **20 ms** 节拍发送,并把幅度上限压到
**450/1024**(这是主机侧的安全策略,不是协议限制)。本固件沿用这两条,实测可用。

## 9. 为什么模拟 `057e:2009` 而不是 `057e:2069`

真实的 Switch 2 Pro 在 USB 上**不是**普通 HID 设备:

- 接口 0 是 HID(描述符映射不正确),**接口 1 是 vendor-specific 的 bulk(0x02/0x82)**,
  另有 USB 音频接口;
- SDL 的 `SDL_hidapi_switch2.c` 用 **libusb 抢接口 1**,通过 bulk 发初始化序列、
  收 64 字节报告;它的 `InitBluetooth()` 直接返回
  `"Nintendo Switch2 controllers not supported over Bluetooth"` —— 这就是
  "Steam 支持 USB 不支持蓝牙"的根因;
- 旧的 `SDL_hidapi_switch.c` 明确把 `0x2069` 排除在外
  (`// The Nintendo Switch 2 Pro uses another driver`)。

在 Windows 上要让 libusb 打开接口 1,需要 WinUSB 绑定(MS OS 2.0 描述符或 INF)。
所以**只做 HID 的 `0x2069` 会被 Steam 完全忽略**;而 `0x2009` 走 hidapi 路径,
Windows 免驱动、握手(`0x80`/`0x81`、`0x01`/`0x21`)都能满足。

代价:C / GL / GR 没有对应的 Switch 1 键位,只能丢弃。

## 10. 已验证 / 未验证

| 项 | 状态 |
|---|---|
| BLE 连接 + 18 步初始化 | ✅ 实机通过 |
| 原生 `0x09` 输入、按键/摇杆 | ✅ 实机通过 |
| 震动(0x02 → `0x0012`) | ✅ 实机通过 |
| 公共 `0x05` 兜底解码 | ⚠️ 代码就绪,实机未走到该分支 |
| 摇杆校准透传 | ⚠️ 单元测试通过,实机待验(看 `cal L factory ok`) |
| 体感(IMU) | ❌ 未实现:原生 0x09 不含 IMU,需改走 0x05 |
| 原生 `0x2069` 人格 | ❌ 未实现:需要 vendor bulk + WinUSB |
