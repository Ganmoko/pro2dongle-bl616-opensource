# Pro2 无线 USB 接收器（LCTech BL616 QFN32）

> 简单来说：把本固件刷入 LCTech BL616 后，BL616 会通过 BLE 无线连接
> Pro2 手柄，再通过 USB 将手柄输入、体感和 HD 振动桥接给电脑或其他主机。

连接方式：

```text
Pro2 手柄  ←── BLE 无线 ──→  LCTech BL616  ←── USB ──→  电脑 / Steam / 主机
```

BL616 在这里相当于 Pro2 手柄的专用无线 USB 接收器。连接主机后，它会枚举为
Nintendo 风格的 `057e:2069` USB 设备；用户无需让电脑自身的蓝牙直接连接手柄。

## 项目来源

本工程由以下两个开源项目迁移、适配而来：

- [LeonChrome/XinHeLianSheng-Pro2-Bridge](https://github.com/LeonChrome/XinHeLianSheng-Pro2-Bridge)：提供 Pro2 / Nintendo 协议与桥接逻辑；
- [sqlCRT/ds5dongle-bl618-opensource](https://github.com/sqlCRT/ds5dongle-bl618-opensource)：提供 LCTech BL616 板级、Bouffalo SDK、蓝牙与 CherryUSB 接入参考。

本仓库将两者组合为面向 LCTech BL616（QFN32）的独立 Bouffalo SDK 工程。
详细来源版本、提交号和许可证说明见 [NOTICE](NOTICE)。

当前 Windows / Steam 实机验证结果、最终固件哈希和已知边界见
[STATUS_CN.md](STATUS_CN.md)。

## 已迁移功能

- BLE 中央设备主动扫描 Pro2；支持名称匹配和厂商 ID `0x0553`。
- 记忆上次连接的 BLE 地址；长按 BOOT 三秒清除并重新配对。
- 发现并使用 FD2 输入、ACK、命令和 CC48 振动四个自定义特征。
- ACK 订阅后依次发送原工程的 15 条初始化命令；初始化完成后只订阅 FD2，
  避免备用通知流令手柄退出 63 字节完整报告模式。
- FD2 按键、四摇杆、背键和运动数据转换为原生 `0x05` 64 字节 USB 报告。
  USB 配置完成后立即以 250 Hz 持续发送并更新序号，不等待主机 vendor 初始化，
  与已验证可用的参考工程 Nintendo 模式保持一致。
- 前 20 帧静止输入自动学习摇杆中心，带死区和物理量程归一化。
- USB 使用 `057e:2069`、Nintendo 字符串和参考工程的精简 Nintendo profile：
  39 字节 vendor/raw HID Report descriptor 及 vendor bulk 共 2 个接口。
  同时提供 Microsoft OS 1.0 与 2.0 描述符，通过兼容 ID 和接口 GUID 将
  vendor 接口绑定到 WinUSB；并兼容旧固件被 Windows 缓存的 `0x20` vendor
  code，以便升级到参考工程使用的 `0xcd` 后自动恢复驱动绑定。
- 转接器使用专属 USB 序列号 `P2DG-BL616-0001` 和设备版本 `bcdDevice=0x0617`。
  Windows 的 Microsoft OS 描述符查询状态按 VID、PID 与 `bcdDevice` 缓存在
  `usbflags` 中，并不包含序列号；独立设备版本可避免复用真实 Pro2
  `bcdDevice=0x0105` 的缓存，确保首次插入时重新查询 WinUSB 描述符。
- 模拟 Switch 2 vendor 初始化应答和校准/序列号 Flash 数据。
- 80 字节 Flash 应答按 BL616 Full-Speed 控制器要求提交为 64 + 16；由于
  `0x02 OUT` 与 `0x82 IN` 共用端点 2 的 VDMA/FIFO，固件会在整个 IN 应答完成后
  才重新挂起下一次 OUT，避免双向传输相互覆盖。
- 将 USB `0x02` 振动帧转换成 Pro2 CC48 33 字节 HD 振动流；20 ms 更新，
  180 ms 保持，结束时发送三帧停止包。
- HID 输入使用端点 `0x81`，振动输出使用独立物理端点 `0x03`；这可避开
  BL616 同编号双向端点共用 VDMA/FIFO 时，OUT 振动令 IN 输入卡死的问题。
- 初始 BLE 连接间隔 15 ms；稳定收到 180 帧后请求 7.5 ms。
- 完整实现 Pro2 的 `0x15` 地址、密钥、挑战和完成配对流程，保存派生 LTK；首次
  使用顶部小配对键完成配对后，转接器重新插拔时可用普通按键唤醒并加密复连。

不包含原工程的通用 HID、XInput、双手柄和 DualSense 实验路径。

`tools/read_ble_diag.py` 同时输出 HID 振动帧的接收、解码与 BLE CC48 写入计数，
无需串口即可判断振动命令停在 USB、解码还是 BLE 层。

## 硬件定义

| 功能 | LCTech BL616 |
|---|---|
| 芯片 | BL616 QFN32 |
| 用户 LED | GPIO27，低电平点亮 |
| BOOT 键 | GPIO2，高电平按下，外部下拉 |
| USB | 板载原生 Type-C |

LED：扫描慢闪、连接中快闪、已就绪常亮、错误慢闪。

## 构建

目录默认采用并列布局：

```text
github/
├── bouffalo_sdk/                 # sqlCRT 的 SDK fork
├── toolchain_gcc_t-head_linux/   # Bouffalo 官方 RISC-V 工具链
└── pro2dongle-bl616-opensource/  # 本工程
```

执行：

```sh
cd pro2dongle-bl616-opensource
git -C ../bouffalo_sdk apply \
  ../pro2dongle-bl616-opensource/patches/bouffalo_sdk_pro2_initiator_scan.patch
./build_lctech616.sh
```

SDK 补丁把白名单主动连接的扫描参数由 60 ms / 30 ms 改为连续
10 ms / 10 ms；这是 Pro2 普通按键唤醒在 BL616 上稳定建连所必需的参数。
已应用时再次执行会提示补丁不适用，无需重复应用。

脚本会自动查找上述并列工具链目录；若工具链安装在别处，请先把其 `bin/`
加入 `PATH`。脚本默认构建与原 Nintendo 身份一致、线材兼容性更好的 USB
Full-Speed 版本。

也可指定 SDK：

```sh
FORCE_FS=1 BL_SDK_BASE=/path/to/bouffalo_sdk make CHIP=bl616 BOARD=bl616dk
```

也可构建 USB High-Speed 版本；该版本会对 bulk 端点使用 512 字节包，并保持
1 ms HID 轮询间隔：

```sh
USB_SPEED=hs ./build_lctech616.sh
```

产物位于 `build/build_out/`，其中烧录需要以下文件：

| 用途 | 文件 | 烧录位置 |
|---|---|---|
| Boot2 | `build/build_out/boot2_bl616_isp_release_v8.1.8.bin` | `0x000000` |
| 分区表 | `build/build_out/partition.bin` | `0x00E000` |
| 应用固件 | `build/build_out/pro2dongle_bl616_bl616.bin` | 由分区表决定（`@partition`） |

仓库根目录的 `flash_prog_cfg.ini` 已经写好这三个文件的路径和地址，建议直接
使用该配置，避免手工填错。不要把同目录下带 `test`、`pairing` 或
`web_config` 后缀的调试固件当作正式固件刷入。

## 烧录

以下流程参考
[sqlCRT/ds5dongle-bl618-opensource](https://github.com/sqlCRT/ds5dongle-bl618-opensource)
的 LCTech BL616 烧录说明，并按本工程的实际产物路径和分区格式调整。

### 准备工作

1. 完成上面的构建，确认 `build/build_out/` 中存在表格列出的三个文件。
2. 下载并解压 [Bouffalo Lab Dev Cube](https://dev.bouffalolab.com/download)。
3. 使用支持数据传输的 USB-C 线；只有供电功能的线无法烧录。
4. Windows 用户可先打开设备管理器，方便确认开发板进入下载模式后新增的
   `COM` 端口。若端口未出现，请先排查数据线、USB 口或驱动。

### 进入 BL616 下载模式

1. 先拔下开发板 USB-C 线。
2. 按住板上的 **BOOT** 键不放。
3. 保持按住 BOOT，将开发板通过 USB-C 接入电脑；看到新的串口后再松开 BOOT。
4. 如果没有出现串口，拔线后重试；不要在固件正常运行状态下直接开始烧录。

### 使用 Dev Cube 烧录

1. 启动 Dev Cube，芯片选择 **BL616/BL618**（部分版本显示为 **BL616**）。
2. 进入 **IOT** 烧录页面，将接口设为 **UART**，刷新并选择刚出现的 `COM` 端口。
3. 优先加载仓库根目录的 `flash_prog_cfg.ini`。它会按以下设置烧录：
   - Boot2：`build/build_out/boot2_bl616_isp_release_v8.1.8.bin`，地址 `0x000000`；
   - Partition：`build/build_out/partition.bin`，地址 `0x00E000`；
   - Firmware：`build/build_out/pro2dongle_bl616_bl616.bin`，地址 `@partition`。
4. 若当前 Dev Cube 版本要求分别选择文件，请严格按上表选择；应用固件地址应由
   分区表解析，不要照搬其他项目的固件地址或 `_nosec.toml` 分区文件。
5. 点击 **Create & Download**（或当前版本中的“创建并下载/下载”），等待进度到
   100% 且日志显示成功。烧录过程中不要拔线或松动 USB 接口。
6. 烧录成功后拔下 USB-C 线，确保不再按住 BOOT，再重新插入；开发板会从 Flash
   正常启动。随后按“使用”一节完成首次手柄配对。

### 命令行烧录（可选）

在已经配置好 Bouffalo SDK 工具链的环境中，也可在仓库根目录执行：

```sh
# Windows 示例
make flash CHIP=bl616 BOARD=bl616dk COMX=COM5

# Linux 示例（端口名按实际情况修改）
make flash CHIP=bl616 BOARD=bl616dk COMX=/dev/ttyACM0
```

命令同样读取根目录的 `flash_prog_cfg.ini`。执行前仍需按上述步骤让开发板进入
下载模式；Linux 若无串口权限，请将当前用户加入 `dialout` 组或按系统规则配置
对应设备权限。

### 常见问题

- **Dev Cube 找不到串口**：确认使用数据线，重新执行“按住 BOOT 再插线”，并在
  Dev Cube 中点击刷新；必要时更换 USB 口。
- **一直停在 `get_boot_info` / 提示握手失败**：开发板通常没有进入下载模式；
  关闭占用串口的终端程序，拔线后按完整步骤重试。
- **提示找不到烧录文件**：保持仓库目录结构不变，并从仓库根目录加载
  `flash_prog_cfg.ini`；若移动过文件，重新构建或修正配置中的相对路径。
- **烧录成功但重新插入后仍显示下载串口**：检查 BOOT 键是否卡住或仍被按下，
  断电后在不按 BOOT 的情况下重新连接。
- **固件启动但手柄不连接**：这通常不是烧录问题。首次连接请长按 Pro2 顶部小
  配对键；更换手柄时长按开发板 BOOT 至少三秒以清除旧配对信息。

## 主机侧协议测试

协议解析、USB 映射、vendor 应答和振动编码均可脱离 SDK 测试：

```sh
make -C tests test
```

## 仅用 USB 读取 BLE 诊断

固件在 WinUSB vendor 接口上接受 `P2DG` 诊断查询，可在没有 USB-TTL
串口线时读取扫描、连接、GATT 发现、订阅和 FD2 输入状态。安装一次 Python
依赖后执行：

```sh
python -m pip install libusb1
python tools/read_ble_diag.py
```

输出中的 `stage` 是当前阶段，`last_error` 和 `last_code` 保留最近一次失败；
`scan_reports`、`candidates`、`connect_successes` 和 `fd2_reports` 可用于区分
广播过滤、建连、GATT 初始化与输入订阅问题。Windows 上接口 1 会通过本工程的
Microsoft OS 描述符绑定 WinUSB。参考工程 profile 还保留 `0x7f` 私有 HID
feature 诊断 Report，因此 `MI_01` 被占用时脚本可以回退到 HID；要测试实际
WinUSB bulk 传输，仍应从系统托盘完全退出 Steam。脚本还会读取 `P2DU` USB
输入计数。USB 诊断会显示 Windows 请求 Microsoft OS 1.0
Compatible ID、属性和 OS 2.0 descriptor set 的次数，用于区分主机未探测与
描述符被拒绝。`P2DV` 跟踪报告会保存最后一条 Steam vendor 命令及应答的长度、
地址和前 20 字节，且不会被诊断查询覆盖。

本版本会显式发现 ACK 和 FD2 的 CCC descriptor，并且只有在初始化命令均收到
ACK、FD2 首帧实际到达之后，才保存手柄地址并将 LED 置为常亮。ACK 订阅会等待
BL616 BLE 栈确认 CCC 写入完成；初始化过程使用独立 ACK 计数，避免把其他异步
GATT 回调误判成命令 ACK。

## 使用

1. 烧录后将板载 Type-C USB 接到电脑或主机。
2. 第一次使用时，长按 Pro2 顶部小配对键令其进入配对状态。
3. 首次配对完成后，固件会保存手柄地址和配对密钥；以后转接器重新插拔后，按
   Pro2 任意普通按键即可唤醒并自动恢复连接，无需再次按顶部配对键。
4. 更换手柄时长按板上 BOOT 键至少三秒。

固件会在板卡没有出厂蓝牙地址时，用芯片 ID 生成稳定的本机地址。首次升级到
包含此修复的版本后，需要用手柄配对键重新配对一次；以后唤醒同一手柄即可由
后台扫描自动连回。

## 网页配置与测试

`web/` 提供不依赖云服务的 WebHID 页面，可读取 BLE/USB 状态、测试按键、摇杆、
回报率与振动，也可设置摇杆死区和振动强度、永久保存设置或清除配对信息。页面
只接受带 `P2CF` 配置协议的本转接器，不会读取或显示保存的配对密钥。

局域网启动：

```sh
python tools/serve_web.py --bind 0.0.0.0 --port 8765
```

启动后按终端打印的 LAN URL 访问。WebHID 要求安全上下文；普通局域网 HTTP
测试需给 Chrome/Edge 使用独立临时配置并加
`--unsafely-treat-insecure-origin-as-secure=<LAN URL>`，正式公开应使用可信 HTTPS。
完整命令和 TLS 用法见 `web/README_CN.md`。当前网页不提供固件升级。

### 网页实机验证状态

以下网页功能已在 Windows、LCTech BL616 转接器和 Pro2 实机上测试通过：

- WebHID 选择并连接转接器；
- BLE/USB 连接与诊断状态显示；
- 全部按键及左右摇杆实时显示；
- USB/BLE 回报率显示；
- 启动和停止振动，振动后输入仍然正常。

“转接器配置”区域已经实现协议和界面，但尚未完成实机验证，包括临时应用或
永久保存摇杆死区、振动强度、恢复默认设置，以及从网页清除配对信息。因此这些
按钮目前按实验功能处理；这不影响已经验证可用的按键、摇杆、回报率、振动和
普通按键唤醒复连主路径。

USB 设备类、产品字符串、版本和序列号采用已验证参考工程的 Nintendo profile；
厂商 Flash 设备信息块包含实机格式的 16 字节序列号、VID/PID、型号参数与配色字段。

没有串口且 USB 诊断接口不可用时，可在一次配对尝试失败后短按并松开 BOOT，
蓝灯会暂停常规节奏并用短闪次数报告最近一次 BLE 错误类别；若没有记录到错误，
则亮一个约 1.2 秒的长闪：

| 短闪次数 | 错误类别 |
|---|---|
| 1 | BLE 协议栈或扫描启动 |
| 2 | 建连启动或连接失败 |
| 3 | GATT 发现启动或超时 |
| 4 | 缺少必需特征 |
| 5 | CCC descriptor 发现失败或缺失 |
| 6 | ACK 订阅、初始化写入或 ACK 超时 |
| 7 | FD2 订阅或首帧超时 |
| 8 | 对端主动断开或其他断开 |
| 9 | BLE 正常，但 USB 尚未完成枚举 |

若第一组为 6 下，还会在约 0.9 秒间隔后显示第二组：1 下表示 ACK 订阅失败，
2 下表示初始化写入失败，3 下表示初始化 ACK 超时，4 下表示 MTU 协商失败。
写入失败或 ACK 超时还会再显示第三组，闪烁次数为失败的初始化命令序号
（1 至 15）。固件会在 GATT 发现前协商 ATT MTU；初始化命令要求 MTU 至少为 31，
因为第 7 条命令包含 28 字节属性数据。

短按诊断不会清除手柄地址；长按三秒仍执行清除并重新扫描。

若第一组为 9 下，第二组表示 USB 到达的最远阶段：1 下表示 USB 控制器已初始化但
未看到主机事件，2 下表示看到连接事件，3 下表示收到总线复位但未完成配置，
4 下表示曾完成配置。BL616 蓝牙初始化会重配外设时钟，因此固件会先等待蓝牙栈
就绪，再恢复 USB 时钟并初始化 USB 设备。

## 验证边界

代码和纯 C 协议测试可在开发机验证；网页的转接器配置写入与持久化仍需实机
回归。最终的 BLE UUID/CCC 布局、Nintendo 主机枚举、实际延迟和 HD 振动仍应
在 LCTech BL616 与目标 Pro2 实机上做回归。
ACK 和 FD2 的 CCC 已采用显式 descriptor discovery；其他尚未实机覆盖的 GATT
布局或 UUID 变化仍需结合上述 USB 诊断输出回归。

来源版本和许可见 `NOTICE`。原 Pro2 工程采用 Apache-2.0；本工程的 BL616
适配来自 GPL-3.0-only 项目，因此组合后的迁移工程按 GPL-3.0-only 发布。
