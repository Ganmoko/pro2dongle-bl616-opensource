# Pro2 网页配置工具

页面使用 WebHID 与转接器 HID 接口直接通信，所有数据停留在浏览器和本机 USB
设备之间。当前支持：

- BLE 连接、广播、配对和错误状态；
- `0x09` / `0x05` 输入报告的按键、摇杆及 USB 回报率测试；
- BLE FD2 新报告回报率；
- 振动测试；
- 摇杆死区、振动强度的临时应用和 EasyFlash 永久保存；
- 带确认口令的配对信息清除。

## 本机运行

`localhost` 被浏览器视为可信来源，可以直接使用 WebHID：

```sh
python tools/serve_web.py --bind 127.0.0.1 --port 8765
```

打开 `http://127.0.0.1:8765/`。

## 局域网临时测试

```sh
python tools/serve_web.py --bind 0.0.0.0 --port 8765
```

普通局域网 HTTP 地址不是 WebHID 安全上下文。Windows 上可用一个隔离的临时
Chrome 配置测试（把地址替换成服务启动时打印的 LAN URL）：

```powershell
& "$env:ProgramFiles\Google\Chrome\Application\chrome.exe" `
  --user-data-dir="$env:TEMP\pro2-webhid" `
  --unsafely-treat-insecure-origin-as-secure="http://192.168.1.10:8765" `
  "http://192.168.1.10:8765/"
```

Edge 可使用同样参数。该方式仅用于可信局域网测试；正式公开时应部署到具有可信
证书的 HTTPS 站点。

服务脚本也支持已有 TLS 证书：

```sh
python tools/serve_web.py --port 8443 \
  --certfile /path/to/fullchain.pem --keyfile /path/to/private-key.pem
```

当前版本不提供网页固件升级。刷写仍使用 BOOT/ISP 流程。

## 当前实机验证状态

已经验证可用：网页连接、BLE/USB 状态、按键、左右摇杆、USB/BLE 回报率和
振动测试。

尚待实机验证：“转接器配置”中的死区和振动强度应用、EasyFlash 永久保存、
恢复默认值及清除配对信息。固件协议和网页界面已经实现，但在完成回归前应将
这些操作视为实验功能。
