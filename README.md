# Pro2 Wireless USB Receiver for LCTech BL616

> In short: flash this firmware to an LCTech BL616, and the BL616 connects
> wirelessly to a Pro2 controller over BLE, then bridges controller input,
> motion, and HD rumble to a PC or other host over USB.

```text
Pro2 controller  <-- BLE wireless -->  LCTech BL616  <-- USB -->  PC / Steam / host
```

The BL616 acts as a dedicated wireless USB receiver for the Pro2 controller.
It enumerates on the host as a Nintendo-style `057e:2069` USB device, so the
host computer does not need to connect to the controller with its own
Bluetooth adapter.

[中文完整说明](README_CN.md)

## Project origins

This firmware combines and adapts work from these two open-source projects:

- [LeonChrome/XinHeLianSheng-Pro2-Bridge](https://github.com/LeonChrome/XinHeLianSheng-Pro2-Bridge), which provides the Pro2/Nintendo protocol and bridge behavior;
- [sqlCRT/ds5dongle-bl618-opensource](https://github.com/sqlCRT/ds5dongle-bl618-opensource), which provides the LCTech BL616 board, Bouffalo SDK, Bluetooth, and CherryUSB integration reference.

The resulting port targets the LCTech BL616 QFN32 board. It makes the BL616
enumerate as a Nintendo-style `057e:2069` USB device and bridges input, motion,
and HD-rumble traffic to a Pro2 controller over BLE. Exact source revisions
and license provenance are recorded in [NOTICE](NOTICE).

The current hardware-validation baseline, firmware hash, and known boundaries
are recorded in [STATUS_CN.md](STATUS_CN.md).

Quick checks:

```sh
make -C tests test
./build_lctech616.sh
```

### Flashing the LCTech BL616

The build creates these required images under `build/build_out/`:

| Purpose | File | Address |
|---|---|---|
| Boot2 | `boot2_bl616_isp_release_v8.1.8.bin` | `0x000000` |
| Partition table | `partition.bin` | `0x00E000` |
| Application | `pro2dongle_bl616_bl616.bin` | `@partition` |

Use the repository's `flash_prog_cfg.ini`; it already selects these images and
addresses. Do not use the `test`, `pairing`, or `web_config` development images
as the normal application firmware.

1. Download and extract [Bouffalo Lab Dev Cube](https://dev.bouffalolab.com/download), and use a USB-C cable that supports data.
2. Unplug the board, hold **BOOT**, reconnect USB-C while keeping BOOT pressed, then release it after the download-mode COM port appears.
3. Start Dev Cube, select **BL616/BL618** (shown as **BL616** in some versions), open the **IOT** page, select **UART**, and choose the new COM port.
4. Load `flash_prog_cfg.ini`. If the Dev Cube version requires separate image selection, use the three files and addresses in the table above; do not copy a firmware address or partition file from another project.
5. Click **Create & Download** and wait for 100% and a success message without disconnecting the board.
6. Unplug the board, make sure BOOT is released, and reconnect it to boot the application from flash.

With a configured Bouffalo SDK toolchain, command-line flashing is also
available after entering download mode:

```sh
# Windows (replace COM5 as needed)
make flash CHIP=bl616 BOARD=bl616dk COMX=COM5

# Linux (replace the device as needed)
make flash CHIP=bl616 BOARD=bl616dk COMX=/dev/ttyACM0
```

If Dev Cube cannot find the port, retry the BOOT-while-plugging sequence and
check the cable, USB port, and driver. A `get_boot_info` or handshake timeout
usually means the board did not enter download mode or another program has the
serial port open. See the [Chinese guide](README_CN.md#烧录) for the full
step-by-step procedure and troubleshooting notes.

BLE diagnostics are also available over the existing WinUSB vendor interface,
so a UART adapter is not required:

```sh
python -m pip install libusb1
python tools/read_ble_diag.py
```

The tool also reads USB transfer, Microsoft OS descriptor requests, the last
Steam vendor command/reply trace, and HID-to-BLE rumble write counters.
The reference-compatible profile exposes a private `0x7f` HID feature report,
so diagnostics can fall back to HID when `MI_01` is busy. Fully exit Steam when
testing the actual WinUSB bulk transport.

A dependency-free WebHID configurator is available in `web/`. It displays
BLE/USB diagnostics, tests buttons, sticks, input rate and rumble, and can
persist stick-deadzone and rumble-strength settings or clear pairing data.
Start the LAN server with:

```sh
python tools/serve_web.py --bind 0.0.0.0 --port 8765
```

WebHID requires a secure context. See `web/README_CN.md` for the isolated
Chromium profile command used for temporary trusted-LAN HTTP testing and for
the HTTPS/TLS option. The page deliberately does not expose pairing keys or
provide browser-based firmware flashing.

### Web configurator hardware-validation status

The following WebHID paths have been tested with the BL616 bridge and a Pro2
on Windows: device connection, BLE/USB status, button and stick display,
USB/BLE input-rate display, and rumble start/stop. They are working.

The adapter-configuration paths are implemented but have not yet been
hardware-validated: applying or persisting stick deadzone and rumble strength,
restoring defaults, and clearing pairing data from the page. Treat these
controls as experimental until that regression is completed. This limitation
does not apply to the already validated controller input and rumble bridge.

The firmware discovers the ACK and FD2 CCC descriptors explicitly and only
saves the peer after the initialization ACK sequence completes and the first
FD2 input report is received.

The firmware continuously sends the native `0x05` report after USB
configuration, matching the working Nintendo mode in the reference project;
input no longer waits for vendor initialization or switches to generic `0x09`.
BL616 submits flash replies as 64 + 16 Full-Speed chunks and serializes
endpoint-2 OUT and IN VDMA use so they cannot overwrite each other. HID input
uses endpoint `0x81` while rumble output uses physical endpoint `0x03`, avoiding
the same shared-VDMA conflict on endpoint 1. Boards
without a factory Bluetooth address derive a stable one from the chip ID so a
previously paired controller can reconnect after a normal wake.

The bridge implements the Pro2 `0x15` address/key/challenge/finalize pairing
exchange and persists the derived LTK. Use the controller's top SYNC button
once for the initial pairing; subsequent adapter power cycles can reconnect an
ordinary button wake with link encryption restored.

The USB identity and descriptors follow the reference project's compact
Nintendo profile: `057e:2069`, a 64-byte two-interface configuration, a
39-byte vendor/raw HID report descriptor, and a WinUSB vendor bulk interface.
It advertises the reference `0xcd` Microsoft OS vendor code while accepting the
legacy bridge value `0x20`, allowing cached Windows devices to migrate without
a manual driver reinstall.
The bridge uses USB serial `P2DG-BL616-0001` and `bcdDevice=0x0617`. Windows
caches Microsoft OS descriptor query state in `usbflags` by VID, PID and
`bcdDevice`, not by serial; the unique revision therefore prevents reuse of a
real Pro2's `bcdDevice=0x0105` cache and forces a fresh WinUSB descriptor probe.
The factory-data response contains the native 16-byte serial, VID/PID, model
parameters and controller colours.

The build script defaults to USB Full-Speed for Nintendo identity and cable
compatibility. Use `USB_SPEED=hs ./build_lctech616.sh` for the High-Speed
variant; both configurations use a 1 ms HID polling interval.

Before building against a clean sibling `bouffalo_sdk`, apply the included
initiator-scan patch once:

```sh
git -C ../bouffalo_sdk apply \
  ../pro2dongle-bl616-opensource/patches/bouffalo_sdk_pro2_initiator_scan.patch
```

It changes whitelist initiation from the SDK's 60 ms / 30 ms scan cadence to
the Pro2-tested continuous 10 ms / 10 ms cadence required for reliable normal
wake reconnects on BL616.

The firmware uses GPIO27 as an active-low status LED, GPIO2 as the active-high
BOOT button, and the board's native USB Type-C connector. Hold BOOT for three
seconds to clear the saved controller address and scan for a new Pro2.

After a failed pairing attempt, a short BOOT press replays the most recent BLE
error category as one to eight short LED pulses (stack/scan, connect, GATT
discovery, missing characteristic, CCC discovery, ACK/init, FD2, or remote
disconnect respectively). One long pulse means that no error is recorded. A
short press does not clear the saved peer. Category six is followed by a second
group: one pulse for ACK subscription, two for an initialization write, or
three for an ACK timeout, and four for an unusable MTU exchange. Write and
timeout failures add a third group with the one-based initialization-command
number. The bridge negotiates ATT MTU before discovery because command seven
contains 28 bytes of attribute data and therefore requires an MTU of at least
31 bytes.

If BLE is healthy but USB has not configured, a short BOOT press shows nine
pulses followed by the furthest USB stage: one for controller initialization,
two for a host-connect event, three for bus reset, or four for a completed
configuration. USB starts only after the BL616 Bluetooth stack is ready because
Bluetooth initialization reconfigures peripheral clocks; the firmware then
restores the USB clock before initializing the device.

This combined port is GPL-3.0-only because its BL616 integration is adapted
from a GPL-3.0-only project. See `NOTICE` for source versions and provenance.
