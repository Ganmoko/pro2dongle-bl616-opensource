const VID = 0x057e;
const PID = 0x2069;
const FEATURE_REPORT_ID = 0x7f;
const FEATURE_PAYLOAD_SIZE = 63;

const STAGES = [
  "启动", "蓝牙栈初始化", "扫描中", "连接中", "已连接", "发现特征",
  "发现描述符", "订阅 ACK", "初始化手柄", "订阅输入", "等待首帧",
  "就绪", "协商 MTU", "自动复连", "首次配对", "恢复加密"
];
const ERRORS = [
  "无", "蓝牙栈初始化", "扫描启动", "停止扫描", "开始连接", "连接失败",
  "发现启动", "发现超时", "缺少特征", "描述符发现启动", "描述符发现超时",
  "缺少 CCC", "订阅 ACK", "初始化写入", "初始化 ACK 超时", "订阅输入",
  "等待输入超时", "连接断开", "MTU 协商", "配对随机数", "配对写入",
  "配对 ACK 超时", "配对应答", "配对加密", "启动链路加密",
  "链路加密超时", "链路加密失败"
];
const WEB_COMMANDS = ["无", "应用", "保存", "恢复默认", "清除配对"];
const WEB_STATUSES = ["空闲", "处理中", "成功", "参数无效", "设备忙", "Flash 写入失败"];
const BUTTONS = ["B", "A", "Y", "X", "L", "R", "ZL", "ZR", "−", "+", "L3", "R3", "↑", "↓", "←", "→", "HOME", "CAP", "GR", "GL", "C"];

const $ = (selector) => document.querySelector(selector);
const $$ = (selector) => [...document.querySelectorAll(selector)];
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

function ascii(text) {
  return new TextEncoder().encode(text);
}

function readU16(bytes, offset) {
  return (bytes[offset] ?? 0) | ((bytes[offset + 1] ?? 0) << 8);
}

function readU32(bytes, offset) {
  return ((bytes[offset] ?? 0) |
    ((bytes[offset + 1] ?? 0) << 8) |
    ((bytes[offset + 2] ?? 0) << 16) |
    ((bytes[offset + 3] ?? 0) << 24)) >>> 0;
}

function readI32(bytes, offset) {
  return readU32(bytes, offset) | 0;
}

function textAt(bytes, offset) {
  let end = offset;
  while (end < bytes.length && bytes[end] !== 0) end++;
  return new TextDecoder().decode(bytes.slice(offset, end));
}

function hasMagic(bytes, magic) {
  if (bytes.length < 4) return false;
  const expected = ascii(magic);
  return expected.every((value, index) => bytes[index] === value);
}

function normalizeFeature(view) {
  let bytes = new Uint8Array(view.buffer, view.byteOffset, view.byteLength);
  if (bytes.length === 64 && bytes[0] === FEATURE_REPORT_ID) bytes = bytes.slice(1);
  return new Uint8Array(bytes);
}

class Pro2HidClient {
  constructor(device) {
    this.device = device;
    this.queue = Promise.resolve();
  }

  async open() {
    if (!this.device.opened) await this.device.open();
  }

  async close() {
    if (this.device.opened) await this.device.close();
  }

  enqueue(operation) {
    const next = this.queue.then(operation, operation);
    this.queue = next.catch(() => {});
    return next;
  }

  exchange(magic, payload = new Uint8Array()) {
    return this.enqueue(async () => {
      if (!this.device.opened) throw new Error("转接器尚未打开");
      const packet = new Uint8Array(FEATURE_PAYLOAD_SIZE);
      packet.set(ascii(magic), 0);
      packet.set(payload.slice(0, FEATURE_PAYLOAD_SIZE - 4), 4);
      await this.device.sendFeatureReport(FEATURE_REPORT_ID, packet);
      await sleep(14);
      const response = normalizeFeature(
        await this.device.receiveFeatureReport(FEATURE_REPORT_ID)
      );
      if (!hasMagic(response, magic === "P2CF" ? "P2CF" : magic)) {
        if (!hasMagic(response, "P2CF")) {
          throw new Error(`设备没有返回 ${magic}，请确认已刷入网页配置固件`);
        }
      }
      return response;
    });
  }

  readBle() { return this.exchange("P2DG"); }
  readAdvertisement() { return this.exchange("P2DA"); }
  readConfig() { return this.exchange("P2CF").then(parseConfig); }

  async command(magic, payload, expectedCommand) {
    let config = parseConfig(await this.exchange(magic, payload));
    for (let attempt = 0; config.lastStatus === 1 && attempt < 30; attempt++) {
      await sleep(35);
      config = await this.readConfig();
    }
    if (config.lastCommand !== expectedCommand || config.lastStatus !== 2) {
      const command = WEB_COMMANDS[config.lastCommand] ?? `命令 ${config.lastCommand}`;
      const status = WEB_STATUSES[config.lastStatus] ?? `状态 ${config.lastStatus}`;
      throw new Error(`${command}失败：${status}`);
    }
    return config;
  }

  applyConfig(rumblePercent, deadzone) {
    return this.command(
      "P2CA",
      new Uint8Array([1, rumblePercent, deadzone & 0xff, deadzone >> 8]),
      1
    );
  }

  saveConfig() { return this.command("P2CS", new Uint8Array(), 2); }
  resetConfig() { return this.command("P2CR", new Uint8Array(), 3); }
  forgetPeer() { return this.command("P2PC", ascii("CLEAR"), 4); }

  async sendRumble(active, sequence = 0) {
    if (!this.device.opened) throw new Error("转接器尚未打开");
    const payload = new Uint8Array(63);
    payload[0] = 0x50 | (sequence & 0x0f);
    const frame = active
      ? new Uint8Array([0x87, 0x41, 0x24, 0x51, 0x20])
      : new Uint8Array([0x87, 0x01, 0x20, 0x11, 0x00]);
    payload.set(frame, 1);
    payload.set(frame, 0x11);
    await this.device.sendReport(0x02, payload);
  }
}

function parseConfig(bytes) {
  if (!hasMagic(bytes, "P2CF") || bytes.length < 14) {
    throw new Error("无效的 P2CF 配置应答");
  }
  return {
    protocol: bytes[4],
    storageVersion: bytes[5],
    rumblePercent: bytes[6],
    deadzone: readU16(bytes, 7),
    capabilities: bytes[9],
    lastCommand: bytes[10],
    lastStatus: bytes[11],
    dirty: bytes[12] !== 0,
    sequence: bytes[13],
    firmware: textAt(bytes, 16)
  };
}

function parseBle(bytes) {
  if (!hasMagic(bytes, "P2DG")) throw new Error("无效的 P2DG 诊断应答");
  const peer = [...bytes.slice(49, 55)].reverse().map((v) => v.toString(16).padStart(2, "0").toUpperCase()).join(":");
  const ack = (bytes[57] ?? 0) | ((bytes[58] ?? 0) << 8) | ((bytes[59] ?? 0) << 16);
  return {
    protocol: bytes[4], stage: bytes[5], error: bytes[6], flags: bytes[7],
    lastCode: readI32(bytes, 8), scanReports: readU32(bytes, 12),
    candidates: readU32(bytes, 16), attempts: readU32(bytes, 20),
    successes: readU32(bytes, 24), disconnects: readU32(bytes, 28),
    fd2: readU32(bytes, 32), peer, initIndex: bytes[55],
    disconnectReason: bytes[56], ack, mtu: readU16(bytes, 61)
  };
}

function parseAdvertisement(bytes) {
  if (!hasMagic(bytes, "P2DA")) throw new Error("无效的 P2DA 诊断应答");
  const signedRssi = bytes[8] > 127 ? bytes[8] - 256 : bytes[8];
  return {
    protocol: bytes[4], flags: bytes[5], captured: (bytes[5] & 1) !== 0,
    rssi: signedRssi, identityFlags: bytes[54] ?? 0,
    pairingStage: bytes[62] ?? 0
  };
}

function unpack12(bytes, offset) {
  return [
    bytes[offset] | ((bytes[offset + 1] & 0x0f) << 8),
    ((bytes[offset + 1] >> 4) & 0x0f) | (bytes[offset + 2] << 4)
  ];
}

function normalizeAxis(value) {
  const result = (value - 2048) / 2047;
  return Math.max(-1, Math.min(1, result));
}

function inputPayload(event) {
  let bytes = new Uint8Array(event.data.buffer, event.data.byteOffset, event.data.byteLength);
  if (bytes.length === 64 && bytes[0] === event.reportId) bytes = bytes.slice(1);
  return bytes;
}

function decodeGeneric(event) {
  const bytes = inputPayload(event);
  if (bytes.length < 9) return null;
  const bitfield = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16);
  const [lx, ly] = unpack12(bytes, 3);
  const [rx, ry] = unpack12(bytes, 6);
  return {
    reportId: 0x09,
    buttons: BUTTONS.map((_, index) => (bitfield & (1 << index)) !== 0),
    axes: [normalizeAxis(lx), normalizeAxis(ly), normalizeAxis(rx), normalizeAxis(ry)]
  };
}

function decodeNative(event) {
  const bytes = inputPayload(event);
  if (bytes.length < 16) return null;
  const b5 = bytes[4], b6 = bytes[5], b7 = bytes[6], b8 = bytes[7];
  const buttons = [
    !!(b5 & 0x04), !!(b5 & 0x08), !!(b5 & 0x01), !!(b5 & 0x02),
    !!(b7 & 0x40), !!(b5 & 0x40), !!(b7 & 0x80), !!(b5 & 0x80),
    !!(b6 & 0x01), !!(b6 & 0x02), !!(b6 & 0x08), !!(b6 & 0x04),
    !!(b7 & 0x02), !!(b7 & 0x01), !!(b7 & 0x08), !!(b7 & 0x04),
    !!(b6 & 0x10), !!(b6 & 0x20), !!(b8 & 0x01), !!(b8 & 0x02), !!(b6 & 0x40)
  ];
  const [lx, nativeLy] = unpack12(bytes, 10);
  const [rx, nativeRy] = unpack12(bytes, 13);
  return {
    reportId: 0x05,
    buttons,
    axes: [normalizeAxis(lx), normalizeAxis(4095 - nativeLy), normalizeAxis(rx), normalizeAxis(4095 - nativeRy)]
  };
}

let client = null;
let statusTimer = null;
let rateTimer = null;
let rumbleTimer = null;
let inputCount = 0;
let inputRateBaseline = { count: 0, time: performance.now() };
let bleRateBaseline = null;
let latestInput = { reportId: null, buttons: BUTTONS.map(() => false), axes: [0, 0, 0, 0] };
let renderPending = false;
let currentConfig = null;
let draftChanged = false;
let toastTimer = null;

function toast(message, kind = "") {
  const element = $("#toast");
  element.textContent = message;
  element.className = `toast show ${kind}`;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { element.className = "toast"; }, 3600);
}

function setConnectionState(state, text) {
  const pill = $("#connectionPill");
  pill.className = `pill ${state}`;
  pill.querySelector("span").textContent = text;
  $("#connectButton").textContent = state === "online" ? "断开" : state === "busy" ? "连接中…" : "连接转接器";
  $("#connectButton").disabled = state === "busy";
}

function setControlsEnabled(enabled) {
  ["#refreshButton", "#rumbleButton", "#stopRumbleButton", "#deadzoneInput",
   "#rumbleInput", "#applyButton", "#saveButton", "#resetButton", "#forgetButton"]
    .forEach((selector) => { $(selector).disabled = !enabled; });
}

function updateConfigUi(config) {
  currentConfig = config;
  $("#deadzoneInput").value = config.deadzone;
  $("#rumbleInput").value = config.rumblePercent;
  $("#deadzoneValue").textContent = config.deadzone;
  $("#rumbleValue").textContent = `${config.rumblePercent}%`;
  $("#configVersion").textContent = `v${config.protocol} / storage ${config.storageVersion}`;
  draftChanged = false;
  const badge = $("#dirtyBadge");
  badge.className = config.dirty ? "badge warn" : "badge success";
  badge.textContent = config.dirty ? "已应用，尚未保存" : "已永久保存";
}

function markDraftChanged() {
  if (!client) return;
  draftChanged = true;
  const badge = $("#dirtyBadge");
  badge.className = "badge warn";
  badge.textContent = "有未应用修改";
}

function updateBleUi(ble, advertisement) {
  const stage = STAGES[ble.stage] ?? `未知阶段 ${ble.stage}`;
  const ready = (ble.flags & 0x10) !== 0;
  $("#metricStage").textContent = stage;
  $("#metricStageHint").textContent = ready ? "手柄输入已经就绪" : `状态标志 0x${ble.flags.toString(16).padStart(2, "0")}`;
  $("#diagVersion").textContent = `P2DG v${ble.protocol}`;
  $("#peerAddress").textContent = ble.peer;
  $("#attMtu").textContent = ble.mtu || "--";
  $("#lastError").textContent = `${ERRORS[ble.error] ?? `错误 ${ble.error}`} (${ble.lastCode})`;
  $("#disconnectReason").textContent = ble.disconnectReason ? `0x${ble.disconnectReason.toString(16).padStart(2, "0")}` : "无";
  $("#scanReports").textContent = ble.scanReports.toLocaleString();
  $("#connectionCounts").textContent = `${ble.successes.toLocaleString()} / ${ble.disconnects.toLocaleString()}`;
  $("#reportCounts").textContent = `${ble.fd2.toLocaleString()} / ${ble.ack.toLocaleString()}`;
  const readyBadge = $("#readyBadge");
  readyBadge.className = ready ? "badge success" : "badge warn";
  readyBadge.textContent = ready ? "READY" : stage;

  if (advertisement.captured) {
    $("#metricRssi").textContent = `${advertisement.rssi} dBm`;
    $("#metricRssiHint").textContent = advertisement.rssi >= -55 ? "信号良好" : advertisement.rssi >= -70 ? "信号一般" : "信号较弱";
  }
  if (advertisement.protocol >= 2) {
    const available = (advertisement.identityFlags & 0x02) !== 0;
    const match = (advertisement.identityFlags & 0x04) !== 0;
    $("#hostMatch").textContent = !available ? "首次配对广播" : match ? "匹配，可普通唤醒" : "不匹配";
  }

  const now = performance.now();
  if (bleRateBaseline) {
    const elapsed = (now - bleRateBaseline.time) / 1000;
    const delta = (ble.fd2 - bleRateBaseline.count) >>> 0;
    if (elapsed > 0.2 && delta < 10000) $("#metricBleRate").textContent = `${(delta / elapsed).toFixed(1)} Hz`;
  }
  bleRateBaseline = { count: ble.fd2, time: now };
}

async function pollStatus(showErrors = false) {
  if (!client) return;
  try {
    const ble = parseBle(await client.readBle());
    const advertisement = parseAdvertisement(await client.readAdvertisement());
    updateBleUi(ble, advertisement);
  } catch (error) {
    if (showErrors) toast(error.message, "error");
  }
}

function scheduleInputRender() {
  if (renderPending) return;
  renderPending = true;
  requestAnimationFrame(() => {
    renderPending = false;
    latestInput.buttons.forEach((active, index) => {
      $("#buttonGrid").children[index]?.classList.toggle("active", active);
    });
    updateStick("#leftStick", "#leftAxes", latestInput.axes[0], latestInput.axes[1]);
    updateStick("#rightStick", "#rightAxes", latestInput.axes[2], latestInput.axes[3]);
    $("#metricReportId").textContent = latestInput.reportId === null ? "等待输入报告" : `当前报告 0x${latestInput.reportId.toString(16).padStart(2, "0")}`;
  });
}

function updateStick(zoneSelector, labelSelector, x, y) {
  const dot = $(`${zoneSelector} .stick-dot`);
  dot.style.left = `${50 + x * 42}%`;
  dot.style.top = `${50 + y * 42}%`;
  $(labelSelector).textContent = `X ${x.toFixed(3)} · Y ${y.toFixed(3)}`;
}

function onInputReport(event) {
  let decoded = null;
  if (event.reportId === 0x09) decoded = decodeGeneric(event);
  if (event.reportId === 0x05) decoded = decodeNative(event);
  if (!decoded) return;
  latestInput = decoded;
  inputCount++;
  scheduleInputRender();
}

function updateInputRate() {
  const now = performance.now();
  const elapsed = (now - inputRateBaseline.time) / 1000;
  if (elapsed > 0) {
    const rate = (inputCount - inputRateBaseline.count) / elapsed;
    if (client) $("#metricUsbRate").textContent = `${Math.max(0, rate).toFixed(1)} Hz`;
  }
  inputRateBaseline = { count: inputCount, time: now };
}

async function connectDevice(device) {
  setConnectionState("busy", "连接中");
  const candidate = new Pro2HidClient(device);
  try {
    await candidate.open();
    const config = await candidate.readConfig();
    client = candidate;
    device.addEventListener("inputreport", onInputReport);
    $("#deviceName").textContent = device.productName || "Nintendo Switch Pro Controller";
    $("#deviceSerial").textContent = device.serialNumber || "浏览器未提供";
    updateConfigUi(config);
    setControlsEnabled(true);
    setConnectionState("online", "已连接");
    statusTimer = setInterval(() => pollStatus(false), 1000);
    clearInterval(rateTimer);
    rateTimer = setInterval(updateInputRate, 500);
    await pollStatus(true);
    toast("已连接 Pro2 BL616 转接器", "success");
  } catch (error) {
    await candidate.close().catch(() => {});
    client = null;
    setConnectionState("offline", "未连接");
    throw error;
  }
}

async function chooseDevice() {
  if (!navigator.hid) throw new Error("当前浏览器或页面来源不支持 WebHID");
  const authorized = (await navigator.hid.getDevices()).filter((device) => device.vendorId === VID && device.productId === PID);
  let device = authorized.length === 1 ? authorized[0] : null;
  if (!device) {
    const devices = await navigator.hid.requestDevice({ filters: [{ vendorId: VID, productId: PID }] });
    device = devices[0];
  }
  if (!device) throw new Error("没有选择设备");
  await connectDevice(device);
}

async function disconnectDevice(showToast = true) {
  clearInterval(statusTimer); statusTimer = null;
  clearInterval(rateTimer); rateTimer = null;
  clearTimeout(rumbleTimer); rumbleTimer = null;
  const active = client;
  client = null;
  if (active) {
    active.device.removeEventListener("inputreport", onInputReport);
    await active.close().catch(() => {});
  }
  setControlsEnabled(false);
  setConnectionState("offline", "未连接");
  $("#metricStage").textContent = "未连接";
  $("#metricBleRate").textContent = "-- Hz";
  $("#metricUsbRate").textContent = "-- Hz";
  bleRateBaseline = null;
  if (showToast) toast("设备已断开");
}

async function applyDraft() {
  if (!client) return null;
  const rumble = Number($("#rumbleInput").value);
  const deadzone = Number($("#deadzoneInput").value);
  const config = await client.applyConfig(rumble, deadzone);
  updateConfigUi(config);
  return config;
}

async function withBusy(button, operation) {
  const oldText = button.textContent;
  button.disabled = true;
  button.textContent = "处理中…";
  try { return await operation(); }
  finally { button.textContent = oldText; button.disabled = !client; }
}

function initButtons() {
  const grid = $("#buttonGrid");
  BUTTONS.forEach((label) => {
    const element = document.createElement("div");
    element.className = "game-button";
    element.textContent = label;
    grid.append(element);
  });
}

function initTabs() {
  $$(".tab").forEach((tab) => tab.addEventListener("click", () => {
    $$(".tab").forEach((item) => item.classList.toggle("active", item === tab));
    $$(".tab-panel").forEach((panel) => panel.classList.toggle("active", panel.id === tab.dataset.tab));
    history.replaceState(null, "", `#${tab.dataset.tab}`);
  }));
  const requested = location.hash.slice(1);
  const tab = $(`.tab[data-tab="${requested}"]`);
  if (tab) tab.click();
}

function initEvents() {
  $("#connectButton").addEventListener("click", async () => {
    try { client ? await disconnectDevice() : await chooseDevice(); }
    catch (error) { toast(error.message, "error"); }
  });
  $("#refreshButton").addEventListener("click", () => pollStatus(true));
  $("#deadzoneInput").addEventListener("input", (event) => { $("#deadzoneValue").textContent = event.target.value; markDraftChanged(); });
  $("#rumbleInput").addEventListener("input", (event) => { $("#rumbleValue").textContent = `${event.target.value}%`; markDraftChanged(); });
  $("#applyButton").addEventListener("click", (event) => withBusy(event.currentTarget, async () => { await applyDraft(); toast("配置已应用，尚未写入 Flash", "success"); }).catch((error) => toast(error.message, "error")));
  $("#saveButton").addEventListener("click", (event) => withBusy(event.currentTarget, async () => {
    if (draftChanged) await applyDraft();
    updateConfigUi(await client.saveConfig());
    toast("配置已永久保存", "success");
  }).catch((error) => toast(error.message, "error")));
  $("#resetButton").addEventListener("click", (event) => {
    if (!confirm("恢复默认死区和振动强度，并永久保存？")) return;
    withBusy(event.currentTarget, async () => { updateConfigUi(await client.resetConfig()); toast("已恢复并保存默认配置", "success"); })
      .catch((error) => toast(error.message, "error"));
  });
  $("#forgetButton").addEventListener("click", (event) => {
    if (!confirm("确定清除已配对手柄吗？下次必须使用顶部小配对键重新配对。")) return;
    withBusy(event.currentTarget, async () => {
      await client.forgetPeer();
      toast("配对信息已清除，转接器开始重新扫描", "success");
      $(".tab[data-tab='status']").click();
      await pollStatus(false);
    }).catch((error) => toast(error.message, "error"));
  });
  $("#rumbleButton").addEventListener("click", async () => {
    try {
      clearTimeout(rumbleTimer);
      await client.sendRumble(true, inputCount);
      rumbleTimer = setTimeout(() => client?.sendRumble(false, inputCount).catch(() => {}), 650);
      toast("振动测试已发送", "success");
    } catch (error) { toast(error.message, "error"); }
  });
  $("#stopRumbleButton").addEventListener("click", async () => {
    clearTimeout(rumbleTimer);
    try { await client.sendRumble(false, inputCount); toast("已停止振动"); }
    catch (error) { toast(error.message, "error"); }
  });

  if (navigator.hid) {
    navigator.hid.addEventListener("disconnect", (event) => {
      if (client?.device === event.device) disconnectDevice(false).then(() => toast("转接器已拔出", "error"));
    });
  }
}

function checkWebHid() {
  if (navigator.hid) return;
  const notice = $("#secureNotice");
  notice.classList.remove("hidden");
  $("#connectButton").disabled = true;
  const reason = !window.isSecureContext
    ? `当前来源 ${location.origin} 不是安全上下文。局域网 HTTP 测试请使用页面说明中的 Chromium 启动参数，或部署可信 HTTPS。`
    : "此浏览器没有 WebHID，请使用最新版 Chrome、Edge 或其他 Chromium 浏览器。";
  $("#secureNoticeText").textContent = reason;
}

initButtons();
initTabs();
initEvents();
checkWebHid();
setControlsEnabled(false);
setConnectionState("offline", "未连接");
scheduleInputRender();

if ("serviceWorker" in navigator && window.isSecureContext) {
  navigator.serviceWorker.register("./sw.js").catch(() => {});
}
