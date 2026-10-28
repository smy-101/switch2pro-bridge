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

v0.3:按键、摇杆(含闪存校准)、电池电压、自动重连、BOOTSEL 长按切换人格、
**网页配置(WebHID)**与人格 flash 持久化。
路线图:震动回传 → 陀螺仪/加速计透传(Steam 体感)→ 耳机口 USB 声卡。

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

## 状态

- [x] 固件编译通过(`build/sw2bridge.uf2`)
- [x] 双人格:Switch Pro v1(默认)+ DS4,BOOTSEL 长按切换
- [x] 网页配置(WebHID):人格切换/保存、实时状态(`web/index.html`)
- [x] 人格 flash 持久化(重刷固件自动回默认)
- [ ] **上板联调(等板子到货)** — 见 `docs/BRINGUP.md`
- [ ] Pro 手柄 BLE 初始化序列实机验证(目前依据 Joy-Con 2 已验证序列外推,详见 BRINGUP 风险清单)
- [ ] 震动 / 体感 / 音频(v2+)

## 为什么可行

- Switch 2 手柄走 **BLE 私有协议**,且**不需要标准蓝牙配对**(扫描→连接→GATT 即用)
- 协议已被完整逆向:`refs/linux-switch2/PROTOCOL.md`(GATT 属性表、初始化序列、报告格式)
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
refs/              参考实现(git 忽略,本地保留)
  linux-switch2/     社区驱动 + PROTOCOL.md ★协议唯一权威来源
  hid-nintendo-valve.c  Valve 主线内核驱动(Pro 映射/USB 侧)
  DS5_Bridge/        Pico 2 W DS4/DualSense 桥(AGPL,输出侧参考)
docs/BRINGUP.md    到板联调手册(必读)
build.sh           WSL 一键构建
```

## 构建(WSL)

```bash
./build.sh          # 首次自动用 pico-sdk/, 工具链在 ~/tools/
# 产物: build/sw2bridge.uf2
```

烧录:按住 Pico 2 W 的 BOOTSEL 插入 USB → 把 `build/sw2bridge.uf2` 拖进弹出的盘符。
WSL 内产物路径可用 `\\wsl.localhost\<distro>\home\smy-101\workspace\self\switch2pro-bridge\build\sw2bridge.uf2`
从 Windows 资源管理器直接访问。

## 联机调试

固件内置 USB CDC 日志(8KB 环形缓冲,IRQ 安全)。上电即输出:
扫描/连接/21 步初始化序列/校准/订阅全过程。用任意串口终端打开对应 COM 口即可。
日志是联调的第一手数据 —— **任何失败步骤,把日志带回来分析**(必要时配合 REA 抓包)。

## 许可

- 本仓库代码:与借用部分兼容地按 **AGPL-3.0** 发布(DS4 descriptor 来自 DS5_Bridge)
- 协议事实不受版权保护,来源均在 refs/ 与上文链接中注明
