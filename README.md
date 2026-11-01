# sw2bridge — Switch 2 Pro Controller → DualShock 4 无线桥

让 **Nintendo Switch 2 Pro 手柄**通过一块 **Raspberry Pi Pico 2 W** 以无线(BLE)方式连到
Windows,电脑端把它识别成 **DualShock 4 (USB 有线)** 手柄:

```
Switch 2 Pro ──BLE(免配对私有协议)──> Pico 2 W ──USB(TinyUSB HID)──> Windows
                                        │
                                        ├─ 默认人格: 任天堂 Switch Pro (057e:2009)
                                        │    → Steam 原生识别,显示与手柄丝印一致的 ABXY 提示
                                        │    → BetterJoy / 加进 Steam 转 XInput
                                        └─ 备用人格: Sony DS4 (054c:09cc)
                                             → DS4Windows / Steam 识别(PS 图形提示)
```

v1.6:摇杆校准透传(SPI `0x603D` 返回由手柄真实校准合成的 Switch 1 校准块,
不再退化成 center 2048 / range 1433)、玩家灯镜像(Steam 的 `0x30` 子命令转成
BLE `0x09/0x07`)、CDC 串口日志改为可关闭(`tusb_config.h` 的 `SW2_CDC_LOG`,
发布版设 0 即得到单接口 HID 设备)。

v1.5:BLE 初始化序列按 linux-switch2 的 `switch2-init.c` 对齐(0x94 传输前导 →
0x27 普通特性集、补 `0x01/0x01`、补 BLE raw selector),并新增**公共报告 0x05 兜底
解码**——原生 0x09 不来时不再全无输入。见下文「BLE 初始化序列」。

v0.5:ATT MTU 改为 512(之前 BTstack 按 1021 缓冲算出 1017,超出 ATT 上限
被手柄静默忽略);初始化顺序改为 BlueZ 插件实机验证过的「ACK CCC → 14 条命令
→ 最后开输入 CCC」,避免 80Hz 输入报告淹没 ACK。
v0.4:修复 BTstack L2CAP/GATT client 未初始化导致的「连上后 ATT 静默」——
这是 v0.3 实机上 `connected` 后 2s 断开、30s 后 `status=0x7f` 的根因。

v0.3:按键、摇杆(含闪存校准)、电池电压、自动重连、BOOTSEL 长按切换人格、
**网页配置(WebHID)**与人格 flash 持久化。
震动:v1.5 起把 Steam/SDL 的 USB 输出报告 `0x10`/`0x01` 里的经典 HD 震动块
转换成 Switch 2 的 5 字节 (freq, amp)×2 帧,以输出报告 `0x02` 写到 BLE 震动特征
`0x0012`(20 ms 节拍、幅度上限 450/1024,与 linux-switch2 一致)。属实验特性,格式
依据内核驱动 `switch2-ff.c` 的 Pro 分支,尚未在 Pro 手柄 BLE 上实测。
路线图:陀螺仪/加速计透传(Steam 体感)→ 耳机口 USB 声卡 → 原生 057e:2069 人格。

## 网页配置(WebHID)

不用装任何软件:用 Chrome / Edge / Opera 打开 **`web/index.html`**(双击即可,也可
托管到任意静态服务器),点「连接设备」选中桥,即可:

- 查看/切换主机人格(Switch Pro ↔ DS4),保存到 flash(重启/换机不变)
- 实时状态:蓝牙链路阶段、手柄电池电压、对端 PID、固件版本

原理与 [DS5Dongle](https://github.com/awalol/DS5Dongle) 同款:两个人格的 HID
report descriptor 里都夹带了厂商 feature report(0xF6~0xF9),浏览器通过
WebHID 的 `sendFeatureReport / receiveFeatureReport` 直接读写,配置存于 flash
最后一个 4KB 扇区(带魔数+CRC,重刷固件自动回默认)。协议定义见
`firmware/cmd.h`;BOOTSEL 长按切换现在也会写入 flash。

## 人格切换(为什么要两个)

Switch 2 Pro 的丝印是任天堂布局(右=A,下=B)。伪装成 DS4 时游戏显示 PS 图形,
与丝印对不上;伪装成 **Switch Pro v1** 时 Steam/支持 Steam Input API 的游戏直接显示
**与手柄完全一致的 ABXY 提示** —— 这是默认人格。按住板载 BOOTSEL ≥1.5 秒切换
到 DS4 人格(USB 重新枚举),再按一次切回。

## BLE 初始化序列与报告路径

固件按 Linux 主线驱动 [`linux-switch2`](https://github.com/novakpetya/linux-switch2)
的 `switch2-init.c` 实测顺序跑 18 步(全部 ACK 驱动):

```
0x94 特性集(传输前导) → 0x11 动作 → 0x0a 震动 → 0x94 使能
→ 0x03/0x0a 选报告 0x09 → FwInfo → NFC 0x0c/0x01 → LED
→ 读左右摇杆工厂校准 → BLE raw selector → 0x27 设置 → 0x27 使能
```

报告侧同时接受两种输入,谁先来就用谁:

| 特征值 | 报告 | 说明 |
|---|---|---|
| `0x000e` | 原生 `0x09` | 首选,带 GL/GR/C 等新键 |
| `0x000a` | 公共 `0x05` | 兜底;BlueZ 插件实测路径最后就停在这里 |

日志会打 `[sw2] first input report on 0x000e (native 0x09)` 或
`... 0x000a (common 0x05 fallback)`,据此判断手柄到底吐的是哪一路。

## 状态

- [x] 固件编译通过(`build/sw2bridge.uf2`)
- [x] 修复 `l2cap_init()`/`gatt_client_init()` 缺失导致的 ATT 静默(v0.4,待上板复测)
- [x] v0.5:MTU 512 + ACK CCC/命令/输入 CCC 的正确初始化顺序(待上板复测)
- [x] 双人格:Switch Pro v1(默认)+ DS4,BOOTSEL 长按切换
- [x] v1.5:初始化序列对齐内核驱动;公共 0x05 兜底解码
- [x] 网页配置(WebHID):人格切换/保存、实时状态(`web/index.html`)
- [x] 人格 flash 持久化(重刷固件自动回默认)
- [ ] **上板联调(等板子到货)** — 见 `docs/BRINGUP.md`
- [ ] Pro 手柄 BLE 初始化序列实机验证(目前依据 Joy-Con 2 已验证序列外推,详见 BRINGUP 风险清单)
- [ ] 震动 / 体感 / 音频(v2+)

## 为什么可行

- Switch 2 手柄走 **BLE 私有协议**,且**不需要标准蓝牙配对**(扫描→连接→GATT 即用)
- 协议已被完整逆向:`refs/linux-switch2/PROTOCOL.md`(GATT 属性表、初始化序列、报告格式);
  本仓库的摘要版见 `docs/PROTOCOL-NOTES.md`(含固件实际依赖的常量与位序,以及
  "为什么模拟 0x2009 而不是 0x2069"的结论)
- Valve 已把驱动合入 Linux 7.4 主线(`refs/hid-nintendo-valve.c`),含 Pro 按键映射
- USB 侧人格化伪装参考了 [DS5_Bridge](https://github.com/SundayMoments/DS5_Bridge)(AGPL-3.0;
  本仓库复用了其 DS4 HID report descriptor 字节,故衍生部分同样受 AGPL 约束)

## 目录

```
firmware/          Pico 2 W 固件 (C, pico-sdk 2.3.0 + BTstack + TinyUSB)
  main.c             主循环: TinyUSB / 看门狗 / LED / 报告泵
  sw2_ble.c/.h       BLE central 状态机: 扫描→连接→初始化序列→订阅报告
  sw2_protocol.h     协议常量(全部来自 PROTOCOL.md / Valve 驱动)
  sw2_decode.c/.h    公共报告 0x05 / Pro 原生报告 0x09 解析 + 摇杆校准
  pro.c/.h           Switch Pro v1 人格: 0x30 报告编码(默认)
  ds4.c/.h           DS4 人格: 报告编码(含 A/B、X/Y 布局对换)
  config.c/.h        配置结构 + flash 持久化(末尾 4KB 扇区, 魔数+CRC)
  cmd.c/.h           网页配置协议: feature report 0xF6~0xF9(见上)
  usb_descriptors.c  双人格设备/HID/字符串描述符 + CDC(日志)复合
  btstack_config.h / tusb_config.h
web/
  index.html         网页配置器(单文件, 零依赖, 双击即用)
  test_web.cjs       页面逻辑雷雾测试(node 直接跑)
tests/
  test_stick_cal.c   校准块打包回归测试(host 上跑, 不需要板子)
  run.sh             一键构建并运行 host 测试
refs/              参考实现(git 忽略,本地保留)
  linux-switch2/     社区驱动 + PROTOCOL.md ★协议唯一权威来源
  hid-nintendo-valve.c  Valve 主线内核驱动(Pro 映射/USB 侧)
  DS5_Bridge/        Pico 2 W DS4/DualSense 桥(AGPL,输出侧参考)
docs/BRINGUP.md    到板联调手册(必读)
docs/PROTOCOL-NOTES.md  协议速查(从 refs/ 摘录,不拉参考仓库也能查)
build.sh           WSL 一键构建
```

## 构建(WSL)

```bash
./build.sh          # 首次自动用 pico-sdk/, 工具链在 ~/tools/
# 产物: build/sw2bridge.uf2

./tests/run.sh      # host 端回归测试(不需要板子)
```

发布版想去掉 USB 串口日志(变成单接口 HID 设备)时,把 `firmware/tusb_config.h`
里的 `SW2_CDC_LOG` 改成 `0`:配置描述符、接口号和日志输出会一起切换,不会出现
"描述符说没有、TinyUSB 却编进去了"的不一致。

烧录:按住 Pico 2 W 的 BOOTSEL 插入 USB → 把 `build/sw2bridge.uf2` 拖进弹出的盘符。
WSL 内产物路径可用 `\\wsl.localhost\<distro>\home\smy\workspace\switch2pro-bridge\build\sw2bridge.uf2`
从 Windows 资源管理器直接访问。

## 联机调试

固件内置 USB CDC 日志(8KB 环形缓冲,IRQ 安全)。上电即输出:
扫描/连接/21 步初始化序列/校准/订阅全过程。用任意串口终端打开对应 COM 口即可。
日志是联调的第一手数据 —— **任何失败步骤,把日志带回来分析**(必要时配合 REA 抓包)。

## 许可

- 本仓库代码:与借用部分兼容地按 **AGPL-3.0** 发布(DS4 descriptor 来自 DS5_Bridge)
- 协议事实不受版权保护,来源均在 refs/ 与上文链接中注明
