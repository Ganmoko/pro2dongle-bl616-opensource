# 当前实机验证状态

状态日期：2026-08-23  
结论：LCTech BL616 QFN32 + Pro2 的 Windows / Steam 桥接主路径已实机验证可用，
可以作为后续开发和回归测试的稳定基线。

## 验证环境与固件身份

- 硬件：LCTech BL616 QFN32 转接器、单只 Pro2 手柄，仅使用 USB 线调试。
- 主机：Windows、Steam；未依赖 USB-TTL 串口。
- USB 身份：`VID=057e`、`PID=2069`、序列号 `P2DG-BL616-0001`。
- USB 设备版本：`bcdDevice=0x0617`。
- USB 接口：MI_00 为 HID，MI_01 为 WinUSB vendor bulk。
- HID 端点：输入 `0x81`，振动输出 `0x03`。
- vendor bulk 端点：输入 `0x82`，输出 `0x02`。
- USB 诊断协议版本：13；BLE 诊断协议版本：10。
- 功能代码基线：`fix/wake-reconnect` 分支本次正式提交。

## 已通过的实机项目

- Windows 能通过 Microsoft OS 1.0 描述符自动将 MI_01 绑定到 WinUSB；诊断显示
  `transport: WinUSB MI_01`，注册表 `osvc={1,205}`，即成功缓存 vendor code
  `0xcd`。
- Steam 能识别设备，手柄页面由“开始设置”变为“详情”，vendor 初始化命令和
  应答正常完成。
- Pro2 能完成 BLE 扫描、连接、GATT 发现、15 条初始化命令、FD2 订阅并进入
  `ready (11)`；实测 ATT MTU 为 247。
- 全部已测试按键映射通过。
- 左、右摇杆轴和方向通过。
- ZL、ZR 等扳机输入通过。
- Steam“测试振动”可令手柄正常振动。
- 振动期间及结束后按键和摇杆输入继续有效，不再发生 HID 输入卡死。
- 一次振动回归诊断记录到 314 个 USB OUT 报告全部成功解码，198 次 BLE CC48
  写入全部成功，拒绝和写入失败均为 0。
- Steam 占用 MI_01 时，`tools/read_ble_diag.py` 可经 HID feature report 回退读取
  BLE、USB、Steam vendor 和振动诊断。
- 首次长按 Pro2 顶部小配对键可完成完整 `0x15` 配对流程，保存手柄地址和派生
  LTK；转接器重新插拔后，用任意普通按键唤醒手柄可恢复加密连接并进入
  `ready (11)`，不需要再次按顶部配对键。
- 普通唤醒实测广告中的 host 地址与转接器持久化本机地址一致，诊断显示
  `host_address_match: True`；正常复连完成 14 条初始化 ACK，首次配对完成 18 条
  ACK（含 4 条配对命令）。
- `make -C tests test` 通过，Full-Speed 完整固件构建通过。

## 已定位并修复的关键问题

1. Windows 不重新查询 WinUSB 描述符：Windows 的 `usbflags` 查询状态按
   VID、PID 和 `bcdDevice` 缓存，不按 USB 序列号区分。转接器改用独立设备
   版本后，Windows 成功重新读取描述符并绑定 WinUSB。
2. Steam 只能显示“开始设置”：MI_01 未绑定 WinUSB 时，Steam 虽能看到 HID，
   但无法完成 Nintendo vendor 初始化；修复驱动绑定后变为“详情”。
3. 点击振动后全部输入失效：BL616 的同编号双向端点共享 VDMA/FIFO，原来的
   `0x81 IN` 与 `0x01 OUT` 会相互覆盖。将 HID OUT 移到独立物理端点 `0x03`
   后，振动和输入可以同时持续工作。
4. 早期输入报告的按键与摇杆映射错误已经按 Pro2 原生 USB 行为修正；当前实机
   回归全部通过。
5. 普通按键唤醒后反复以 HCI `0x3e` 断开：实现 Pro2 `0x15` 地址交换、A1/B1
   密钥交换、AES-128 挑战和配对完成流程，持久化 LTK 并在复连时恢复链路加密；
   同时把白名单主动连接扫描窗口改为连续 10 ms / 10 ms。实测重新插拔转接器后
   可由普通按键稳定唤醒、复连并进入 ready。

## 当前固件产物

文件：`build/build_out/pro2dongle_bl616_bl616.bin`

SHA-256：

```text
04929BE16206810D9156408C84EDC67AC05AE000F4F167DEADEB1F09E03E7F63
```

构建命令：

```sh
git -C ../bouffalo_sdk apply \
  ../pro2dongle-bl616-opensource/patches/bouffalo_sdk_pro2_initiator_scan.patch
./build_lctech616.sh
```

## 尚未宣称完成的范围

- 当前只验证了单只 Pro2，不包含通用 HID、XInput、双手柄或 DualSense 路径。
- 尚未在 Nintendo Switch 2 主机上进行实机兼容性测试。
- 运动传感器数据、长时间稳定性、不同 Windows 电脑以及不同 Pro2 固件版本尚未
  做完整矩阵测试。
- 当前验证的是 USB Full-Speed 构建；High-Speed 变体未完成同等实机回归。

## 后续修改注意事项

- 如果再次改变 USB 配置描述符或端点布局，应同步递增 `bcdDevice`，防止 Windows
  复用旧的设备和 OS 描述符缓存。
- BL616 上不要同时使用同一个非零端点编号的 IN/OUT VDMA；若必须保持协议端点
  编号，应在固件内严格串行化双向传输。
- Steam 开启时诊断显示 `transport: HID feature fallback` 属于预期现象，因为
  Steam 会占用 MI_01；测试 WinUSB 原始传输时应完全退出 Steam。
- 后续回归至少应覆盖：Steam 显示“详情”、全按键、双摇杆、双扳机、振动，以及
  振动后输入仍持续有效。
