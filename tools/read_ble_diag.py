#!/usr/bin/env python3
"""Read Pro2 BLE diagnostics from the dongle's WinUSB vendor interface."""

from __future__ import annotations

import argparse
import sys
import time

try:
    import usb1
except ImportError:
    usb1 = None

try:
    import hid
except ImportError:
    hid = None

USB_EXCEPTIONS = (RuntimeError,) if usb1 is None else (RuntimeError, usb1.USBError)


VID = 0x057E
PID = 0x2069
INTERFACE = 1
EP_OUT = 0x02
EP_IN = 0x82
REPORT_SIZE = 64

STAGES = (
    "boot",
    "stack_init",
    "scanning",
    "connecting",
    "connected",
    "characteristic_discovery",
    "descriptor_discovery",
    "ack_subscribe",
    "initializing",
    "fd2_subscribe",
    "waiting_fd2",
    "ready",
    "mtu_exchange",
    "auto_connect",
    "pairing",
    "securing",
)

ERRORS = (
    "none",
    "stack_init",
    "scan_start",
    "scan_stop",
    "connect_start",
    "connect",
    "discovery_start",
    "discovery_timeout",
    "characteristic_missing",
    "descriptor_start",
    "descriptor_timeout",
    "ccc_missing",
    "ack_subscribe",
    "init_write",
    "init_ack_timeout",
    "fd2_subscribe",
    "fd2_timeout",
    "disconnected",
    "mtu_exchange",
    "pairing_random",
    "pairing_write",
    "pairing_ack_timeout",
    "pairing_response",
    "pairing_crypto",
    "security_start",
    "security_timeout",
    "security_change",
)

FLAG_NAMES = (
    (0x01, "saved_peer"),
    (0x02, "scanning"),
    (0x04, "connecting"),
    (0x08, "connected"),
    (0x10, "ready"),
    (0x20, "waiting_first_fd2"),
    (0x40, "candidate_directed"),
    (0x80, "directed_seen"),
)

ADVERTISEMENT_EVENT_NAMES = (
    "connectable_undirected",
    "connectable_directed",
    "scannable_undirected",
    "nonconnectable_undirected",
    "scan_response",
)

PAIRING_STAGES = (
    "none",
    "address",
    "key",
    "challenge",
    "finalize",
    "complete",
)


def name_at(names, index: int) -> str:
    return names[index] if index < len(names) else f"unknown_{index}"


def u16(data: bytes, offset: int) -> int:
    return int.from_bytes(data[offset : offset + 2], "little")


def u32(data: bytes, offset: int) -> int:
    return int.from_bytes(data[offset : offset + 4], "little")


def i32(data: bytes, offset: int) -> int:
    return int.from_bytes(data[offset : offset + 4], "little", signed=True)


def open_dongle(context):
    for device in context.getDeviceIterator(skip_on_error=True):
        if device.getVendorID() != VID or device.getProductID() != PID:
            continue
        try:
            return device.open()
        except usb1.USBError as error:
            raise RuntimeError(
                "057e:2069 is enumerated but cannot be opened; "
                "check that interface MI_01 is using the WinUSB driver "
                f"({error})"
            ) from error
    raise RuntimeError(
        "057e:2069 is not enumerated; verify the new firmware is flashed "
        "and the board is running normally rather than in ISP mode"
    )


def read_bulk_report(magic: bytes, timeout_ms: int) -> bytes:
    query = magic + bytes(REPORT_SIZE - len(magic))
    with usb1.USBContext() as context:
        handle = open_dongle(context)
        try:
            try:
                handle.claimInterface(INTERFACE)
            except usb1.USBError as error:
                raise RuntimeError(
                    "057e:2069 opened but interface MI_01 could not be claimed; "
                    f"close other programs and verify the WinUSB driver ({error})"
                ) from error
            handle.bulkWrite(EP_OUT, query, timeout=timeout_ms)
            report = bytes(handle.bulkRead(EP_IN, REPORT_SIZE, timeout=timeout_ms))
        finally:
            try:
                handle.releaseInterface(INTERFACE)
            except usb1.USBError:
                pass
            handle.close()
    if len(report) != REPORT_SIZE or report[:4] != magic:
        raise RuntimeError(f"invalid diagnostic reply: {report.hex(' ')}")
    return report


def make_flash_read_command(address: int) -> bytearray:
    command = bytearray(
        (
            0x02,
            0x91,
            0x00,
            0x01,
            0x00,
            0x08,
            0x00,
            0x00,
            0x00,
            0x00,
            0x00,
            0x00,
            0x00,
            0x00,
            0x00,
            0x00,
        )
    )
    command[12:16] = address.to_bytes(4, "little")
    return command


def read_flash_reply(handle, address: int, timeout_ms: int, read_layout: str):
    """Issue one flash read on an already claimed MI_01 handle."""
    command = make_flash_read_command(address)
    timings_ms = []
    started = time.monotonic()
    try:
        written = handle.bulkWrite(EP_OUT, command, timeout=timeout_ms)
    except usb1.USBError as error:
        elapsed = (time.monotonic() - started) * 1000
        raise RuntimeError(f"bulk OUT failed after {elapsed:.1f} ms ({error})") from error

    chunks = []
    request_sizes = (80,) if read_layout == "single" else (64, 16)
    for index, request_size in enumerate(request_sizes, 1):
        started = time.monotonic()
        try:
            chunk = bytes(handle.bulkRead(EP_IN, request_size, timeout=timeout_ms))
        except usb1.USBError as error:
            elapsed = (time.monotonic() - started) * 1000
            raise RuntimeError(
                f"bulk IN chunk {index}/{len(request_sizes)} "
                f"(request {request_size}) failed after {elapsed:.1f} ms ({error})"
            ) from error
        timings_ms.append((time.monotonic() - started) * 1000)
        chunks.append(chunk)

    reply = b"".join(chunks)
    if len(reply) != 80:
        raise RuntimeError(f"expected an 80-byte reply, received {len(reply)}")
    if reply[0] != 0x02 or reply[3] != 0x01:
        raise RuntimeError("reply command header does not match a flash read")
    if int.from_bytes(reply[12:16], "little") != address:
        raise RuntimeError("reply address does not match the requested address")
    return written, chunks, reply, timings_ms


def probe_flash_read(
    address: int, timeout_ms: int, read_layout: str, repeat: int
) -> None:
    """Issue flash reads while keeping one claimed MI_01 handle open."""
    results = []
    with usb1.USBContext() as context:
        handle = open_dongle(context)
        try:
            try:
                handle.claimInterface(INTERFACE)
            except usb1.USBError as error:
                raise RuntimeError(
                    "057e:2069 opened but interface MI_01 could not be claimed; "
                    f"close Steam and browser gamepad pages ({error})"
                ) from error
            for index in range(repeat):
                try:
                    results.append(
                        read_flash_reply(handle, address, timeout_ms, read_layout)
                    )
                except RuntimeError as error:
                    raise RuntimeError(
                        f"flash read {index + 1}/{repeat} failed: {error}"
                    ) from error
        finally:
            try:
                handle.releaseInterface(INTERFACE)
            except usb1.USBError:
                pass
            handle.close()

    print("transport: WinUSB MI_01 flash probe")
    print(f"flash_address: 0x{address:08x}")
    print(f"repeat: {repeat}")
    for index, (written, chunks, reply, timings_ms) in enumerate(results, 1):
        print(f"read[{index}].write_bytes: {written}")
        print(
            f"read[{index}].chunks: "
            + " + ".join(str(len(chunk)) for chunk in chunks)
        )
        print(
            f"read[{index}].timings_ms: "
            + " + ".join(f"{elapsed:.1f}" for elapsed in timings_ms)
        )
        if index == 1 or index == repeat:
            print(f"read[{index}].reply: " + reply.hex(" "))


def read_hid_report(magic: bytes) -> bytes:
    if hid is None:
        raise RuntimeError(
            "MI_01 is not WinUSB and HID fallback is unavailable; "
            "run `python -m pip install hidapi`"
        )
    last_error = None
    for info in hid.enumerate(VID, PID):
        device = hid.device()
        try:
            device.open_path(info["path"])
            selector = bytes([0x7F]) + magic + bytes(REPORT_SIZE - 1 - len(magic))
            device.send_feature_report(selector)
            raw = bytes(device.get_feature_report(0x7F, REPORT_SIZE))
            if len(raw) >= REPORT_SIZE and raw[0] == 0x7F and raw[1:5] == magic:
                # The report ID consumes byte zero; reconstruct the reserved
                # final diagnostic byte to keep the common 64-byte format.
                return raw[1:REPORT_SIZE] + b"\x00"
        except (OSError, ValueError) as error:
            last_error = error
        finally:
            device.close()
    detail = f" ({last_error})" if last_error else ""
    raise RuntimeError(
        f"057e:2069 HID {magic.decode(errors='replace')} feature report not found{detail}"
    )


def monitor_hid_input(duration_s: float, usb_protocol_version: int = 0) -> None:
    if hid is None:
        raise RuntimeError("HID monitoring requires `python -m pip install hidapi`")

    last_error = None
    for info in hid.enumerate(VID, PID):
        device = hid.device()
        try:
            device.open_path(info["path"])
            started = time.monotonic()
            deadline = started + duration_s
            reports = 0
            changes = 0
            first_sequence = None
            last_sequence = None
            last_controls = None
            print(
                f"\nMonitoring raw HID input for {duration_s:g} seconds; "
                "press buttons and move both sticks..."
            )
            while time.monotonic() < deadline:
                raw = bytes(device.read(REPORT_SIZE, 200))
                if not raw or raw[0] not in (0x05, 0x09):
                    continue
                reports += 1
                if raw[0] == 0x09 and usb_protocol_version >= 12:
                    sequence = raw[63] if len(raw) >= REPORT_SIZE else None
                else:
                    sequence = raw[1] if len(raw) > 1 else None
                if first_sequence is None:
                    first_sequence = sequence
                last_sequence = sequence
                if raw[0] == 0x09:
                    if usb_protocol_version >= 12:
                        buttons = raw[1:4]
                        sticks = raw[4:10]
                    else:
                        buttons = raw[3:6]
                        sticks = raw[6:12]
                else:
                    buttons = raw[5:9]
                    sticks = raw[11:17]
                controls = buttons + sticks
                if controls != last_controls:
                    changes += 1
                    if changes <= 24:
                        print(
                            f"hid_change[{changes}]: report=0x{raw[0]:02x} "
                            f"seq={sequence} buttons={buttons.hex(' ')} "
                            f"sticks={sticks.hex(' ')}"
                        )
                    last_controls = controls
            elapsed = time.monotonic() - started
            print(f"hid_reports: {reports}")
            print(f"hid_elapsed_seconds: {elapsed:.3f}")
            print(
                "hid_report_rate_hz: "
                f"{reports / elapsed:.1f}" if elapsed > 0 else "unavailable"
            )
            print(f"hid_control_changes: {changes}")
            print(f"hid_sequence: {first_sequence} -> {last_sequence}")
            return
        except Exception as error:
            last_error = error
        finally:
            device.close()

    detail = f": {last_error}" if last_error else ""
    raise RuntimeError(f"could not monitor 057e:2069 HID input{detail}")


def monitor_ble_rate(duration_s: float, read_report) -> None:
    """Measure fresh BLE notifications from the firmware's FD2 counter."""
    sample_s = 1.0
    previous = read_report()
    previous_count = u32(previous, 32)
    previous_time = time.monotonic()
    started = previous_time
    rates = []
    total_reports = 0
    sample_index = 0

    print(f"\nMonitoring fresh BLE input for {duration_s:g} seconds...")
    while previous_time - started < duration_s:
        target = min(started + (sample_index + 1) * sample_s, started + duration_s)
        remaining = target - time.monotonic()
        if remaining > 0:
            time.sleep(remaining)
        current = read_report()
        current_time = time.monotonic()
        current_count = u32(current, 32)
        elapsed = current_time - previous_time
        delta = (current_count - previous_count) & 0xFFFFFFFF
        rate = delta / elapsed if elapsed > 0 else 0.0
        rates.append(rate)
        total_reports += delta
        sample_index += 1
        print(
            f"ble_rate[{sample_index}]: {rate:.1f} Hz "
            f"({delta} reports / {elapsed:.3f} s)"
        )
        previous = current
        previous_count = current_count
        previous_time = current_time

    total_elapsed = previous_time - started
    average = total_reports / total_elapsed if total_elapsed > 0 else 0.0
    print(f"ble_reports: {total_reports}")
    print(f"ble_rate_average_hz: {average:.1f}")
    if rates:
        print(f"ble_rate_min_hz: {min(rates):.1f}")
        print(f"ble_rate_max_hz: {max(rates):.1f}")
    print(f"ble_elapsed_seconds: {total_elapsed:.3f}")


def print_report(data: bytes, raw: bool) -> None:
    flags = data[7]
    enabled_flags = [name for bit, name in FLAG_NAMES if flags & bit]
    peer = ":".join(f"{byte:02X}" for byte in reversed(data[49:55]))
    handles = {
        "fd2_value": u16(data, 36),
        "fd2_ccc": u16(data, 38),
        "ack_value": u16(data, 40),
        "ack_ccc": u16(data, 42),
        "command_value": u16(data, 44),
        "rumble_value": u16(data, 46),
    }

    print(f"protocol_version: {data[4]}")
    print(f"stage: {name_at(STAGES, data[5])} ({data[5]})")
    print(f"last_error: {name_at(ERRORS, data[6])} ({data[6]})")
    print(f"last_code: {i32(data, 8)}")
    print(f"flags: {', '.join(enabled_flags) if enabled_flags else 'none'}")
    print(f"scan_reports: {u32(data, 12)}")
    print(f"candidates: {u32(data, 16)}")
    print(f"connect_attempts: {u32(data, 20)}")
    print(f"connect_successes: {u32(data, 24)}")
    print(f"disconnects: {u32(data, 28)}")
    print(f"fd2_reports: {u32(data, 32)}")
    ack_reports = (
        int.from_bytes(data[57:60], "little")
        if data[4] >= 4
        else u32(data, 57)
    )
    print(f"ack_reports: {ack_reports}")
    print(f"att_mtu: {u16(data, 61)}")
    print(f"peer: {peer}/{data[48]}")
    print(f"init_index: {data[55]}")
    print(f"last_disconnect_reason: {data[56]}")
    address_flag_byte = data[60] if data[4] >= 4 else data[63]
    if data[4] >= 2:
        address_flags = [
            name
            for bit, name in (
                (0x01, "restored"),
                (0x02, "captured"),
                (0x04, "derived"),
                (0x08, "saved"),
                (0x10, "controller_set"),
                (0x20, "host_synced"),
            )
            if address_flag_byte & bit
        ]
        print(
            "local_address: "
            + (", ".join(address_flags) if address_flags else "unavailable")
        )
    if data[4] >= 3:
        mtu_path = []
        if address_flag_byte & 0x40:
            mtu_path.append("peer")
        if address_flag_byte & 0x80:
            mtu_path.append("local_request")
        print("mtu_negotiation: " + (", ".join(mtu_path) if mtu_path else "pending"))
    if data[4] >= 6:
        auto_flags = [
            name
            for bit, name in (
                (0x01, "selected"),
                (0x02, "armed"),
                (0x04, "start_failed"),
            )
            if data[63] & bit
        ]
        print("auto_connect: " + (", ".join(auto_flags) if auto_flags else "disabled"))
    if data[4] >= 7:
        print("initiator_scan: 10 ms / 100% duty")
    print(
        "handles: "
        + ", ".join(f"{key}=0x{value:04x}" for key, value in handles.items())
    )
    if data[6] == 8:
        missing = [
            name
            for bit, name in (
                (1, "FD2"),
                (2, "ACK"),
                (4, "command"),
                (8, "rumble"),
            )
            if i32(data, 8) & bit
        ]
        print("missing_characteristics: " + ", ".join(missing))
    if raw:
        print("raw: " + data.hex(" "))


def print_advertisement_report(data: bytes, raw: bool) -> None:
    flags = data[5]
    copied = min(data[10], 32)
    payload = data[22 : 22 + copied]
    peer = ":".join(f"{byte:02X}" for byte in reversed(data[16:22]))
    event_type = data[6]
    event_name = name_at(ADVERTISEMENT_EVENT_NAMES, event_type)
    rssi = int.from_bytes(data[8:9], "little", signed=True)

    print("\nBLE advertisement diagnostics:")
    print(f"advertisement_protocol_version: {data[4]}")
    print(f"captured: {bool(flags & 0x01)}")
    print(f"saved_peer_match: {bool(flags & 0x02)}")
    print(f"candidate_match: {bool(flags & 0x04)}")
    print(f"event_type: {event_name} ({event_type})")
    print(f"event_type_mask: 0x{data[11]:02x}")
    print(f"connectable: {bool(flags & 0x10)}")
    print(f"directed: {bool(flags & 0x08)}")
    print(f"rssi: {rssi} dBm")
    print(f"peer: {peer}/{data[7]}")
    print(f"matching_reports: {u32(data, 12)}")
    print(f"payload_length: {data[9]} (captured {copied})")
    print("payload: " + (payload.hex(" ") if payload else "none"))

    structures = []
    offset = 0
    while offset < len(payload):
        length = payload[offset]
        if length == 0:
            break
        end = offset + 1 + length
        if end > len(payload) or length < 1:
            structures.append("truncated")
            break
        kind = payload[offset + 1]
        value = payload[offset + 2 : end]
        detail = value.hex(" ")
        if kind in (0x08, 0x09):
            detail = value.decode("utf-8", errors="replace")
        elif kind == 0xFF and len(value) >= 2:
            detail = f"company=0x{u16(value, 0):04x} data={value[2:].hex(' ')}"
        structures.append(f"0x{kind:02x}={detail}")
        offset = end
    print("ad_structures: " + (", ".join(structures) if structures else "none"))
    if data[4] >= 2:
        identity_flags = data[54]
        local_identity = ":".join(
            f"{byte:02X}" for byte in reversed(data[56:62])
        )
        advertised_host = "unavailable"
        if len(payload) >= 23 and identity_flags & 0x02:
            advertised_host = ":".join(
                f"{byte:02X}" for byte in reversed(payload[17:23])
            )
        print(
            "local_identity: "
            + (f"{local_identity}/{data[55]}" if identity_flags & 0x01 else "unavailable")
        )
        print(f"advertised_host: {advertised_host}")
        print(f"host_address_match: {bool(identity_flags & 0x04)}")
    if data[4] >= 3:
        pairing_flags = [
            name
            for bit, name in (
                (0x08, "required"),
                (0x10, "started"),
                (0x20, "address_ok"),
                (0x40, "challenge_ok"),
                (0x80, "committed"),
            )
            if data[63] & bit
        ]
        print(f"pairing_stage: {name_at(PAIRING_STAGES, data[62])} ({data[62]})")
        print("pairing_flags: " + (", ".join(pairing_flags) if pairing_flags else "none"))
    if raw:
        print("advertisement_raw: " + data.hex(" "))


def print_usb_report(data: bytes, raw: bool) -> None:
    flags = data[5]
    enabled_flags = [
        name
        for bit, name in (
            (0x01, "configured"),
            (0x02, "hid_busy"),
            (0x04, "input_pending"),
            (0x08, "vendor_busy"),
            (0x10, "generic_report_enabled"),
            (0x20, "host_input_started"),
            (0x40, "report_05_selected"),
            (0x80, "vendor_out_armed"),
        )
        if flags & bit
    ]
    print("\nUSB input diagnostics:")
    print(f"usb_protocol_version: {data[4]}")
    print(f"usb_flags: {', '.join(enabled_flags) if enabled_flags else 'none'}")
    print(f"usb_stage: {data[6]}")
    print(f"input_sequence: {data[7]}")
    print(f"last_start_result: {i32(data, 8)}")
    print(f"input_updates: {u32(data, 12)}")
    print(f"periodic_ticks: {u32(data, 16)}")
    print(f"hid_starts: {u32(data, 20)}")
    print(f"hid_completions: {u32(data, 24)}")
    print(f"hid_failures: {u32(data, 28)}")
    print(f"hid_last_complete_bytes: {u32(data, 32)}")
    print(f"hid_out_reports: {u32(data, 36)}")
    print(f"vendor_out_reports: {u32(data, 40)}")
    print(f"vendor_in_starts: {u32(data, 44)}")
    print(f"vendor_in_completions: {u32(data, 48)}")
    if data[4] >= 2:
        print(f"ms_os_10_compat_requests: {u32(data, 52)}")
        print(f"ms_os_10_property_requests: {u32(data, 56)}")
        print(f"ms_os_20_descriptor_requests: {u32(data, 60)}")
    else:
        print("last_input_prefix: " + data[52:64].hex(" "))
    if raw:
        print("usb_raw: " + data.hex(" "))


def print_vendor_report(data: bytes, raw: bool) -> None:
    flags = data[5]
    print("\nSteam vendor diagnostics:")
    print(f"vendor_protocol_version: {data[4]}")
    print(f"last_command_valid: {bool(flags & 0x01)}")
    print(f"last_reply_valid: {bool(flags & 0x02)}")
    print(f"last_command_length: {u16(data, 6)}")
    print(f"last_reply_length: {u16(data, 8)}")
    print(f"last_command: 0x{data[10]:02x}")
    print(f"last_argument: 0x{data[11]:02x}")
    print(f"last_address: 0x{u32(data, 12):08x}")
    print(f"vendor_out_at_trace: {u32(data, 16)}")
    print(f"vendor_commands_dropped_busy: {u32(data, 20)}")
    print("last_command_prefix: " + data[24:44].hex(" "))
    print("last_reply_prefix: " + data[44:64].hex(" "))
    if raw:
        print("vendor_raw: " + data.hex(" "))


def print_rumble_report(data: bytes, raw: bool) -> None:
    flags = data[5]
    enabled_flags = [
        name for bit, name in ((0x01, "active"), (0x02, "stopping"))
        if flags & bit
    ]
    print("\nRumble diagnostics:")
    print(f"rumble_protocol_version: {data[4]}")
    print(f"rumble_flags: {', '.join(enabled_flags) if enabled_flags else 'idle'}")
    print(f"packet_id: {data[6]}")
    print(f"last_usb_length: {data[7]}")
    print(f"usb_reports: {u32(data, 8)}")
    print(f"decoded_reports: {u32(data, 12)}")
    print(f"rejected_reports: {u32(data, 16)}")
    print(f"ble_write_attempts: {u32(data, 20)}")
    print(f"ble_write_successes: {u32(data, 24)}")
    print(f"ble_write_failures: {u32(data, 28)}")
    print(f"last_write_result: {i32(data, 32)}")
    print("last_usb_prefix: " + data[36:52].hex(" "))
    print("last_ble_prefix: " + data[52:64].hex(" "))
    if raw:
        print("rumble_raw: " + data.hex(" "))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--timeout", type=int, default=2000, help="USB timeout in ms"
    )
    parser.add_argument("--raw", action="store_true", help="also print the raw packet")
    parser.add_argument(
        "--monitor",
        type=float,
        default=0,
        metavar="SECONDS",
        help="read raw 0x05/0x09 HID input reports while controls are operated",
    )
    parser.add_argument(
        "--ble-rate",
        type=float,
        default=0,
        metavar="SECONDS",
        help="measure fresh BLE FD2 reports in one-second windows",
    )
    parser.add_argument(
        "--probe-flash",
        type=lambda value: int(value, 0),
        metavar="ADDRESS",
        help="send one Steam-style flash read and inspect its 80-byte reply",
    )
    parser.add_argument(
        "--probe-read",
        choices=("split", "single"),
        default="split",
        help="flash-probe receive layout: two 64+16 reads or one 80-byte read",
    )
    parser.add_argument(
        "--probe-repeat",
        type=int,
        default=1,
        metavar="COUNT",
        help="repeat flash reads on one claimed MI_01 handle",
    )
    args = parser.parse_args()
    if args.ble_rate < 0:
        parser.error("--ble-rate must not be negative")
    if args.probe_flash is not None:
        if usb1 is None:
            print("flash probe requires `python -m pip install libusb1`", file=sys.stderr)
            return 1
        if not 0 <= args.probe_flash <= 0xFFFFFFFF:
            parser.error("--probe-flash address must fit in 32 bits")
        if args.probe_repeat < 1:
            parser.error("--probe-repeat must be at least 1")
        try:
            probe_flash_read(
                args.probe_flash,
                args.timeout,
                args.probe_read,
                args.probe_repeat,
            )
        except USB_EXCEPTIONS as error:
            print(f"Steam flash probe failed: {error}", file=sys.stderr)
            return 1
        return 0
    bulk_error = None
    try:
        if usb1 is None:
            raise RuntimeError("libusb1 is not installed")
        report = read_bulk_report(b"P2DG", args.timeout)
        source = "WinUSB MI_01"
    except USB_EXCEPTIONS as error:
        bulk_error = error
        try:
            report = read_hid_report(b"P2DG")
            source = "HID feature fallback"
        except RuntimeError as hid_error:
            print(f"BLE diagnostic read failed: {bulk_error}; {hid_error}", file=sys.stderr)
            return 1
    print(f"transport: {source}")
    print_report(report, args.raw)
    try:
        if bulk_error is None:
            advertisement_report = read_bulk_report(b"P2DA", args.timeout)
        else:
            advertisement_report = read_hid_report(b"P2DA")
        print_advertisement_report(advertisement_report, args.raw)
    except USB_EXCEPTIONS as error:
        print(f"\nBLE advertisement diagnostics unavailable: {error}")
    usb_protocol_version = 0
    if bulk_error is None:
        try:
            usb_report = read_bulk_report(b"P2DU", args.timeout)
            usb_protocol_version = usb_report[4]
            print_usb_report(usb_report, args.raw)
        except USB_EXCEPTIONS as error:
            print(f"USB diagnostic read failed: {error}", file=sys.stderr)
    else:
        try:
            usb_report = read_hid_report(b"P2DU")
            usb_protocol_version = usb_report[4]
            print_usb_report(usb_report, args.raw)
        except RuntimeError as error:
            print(f"\nUSB input diagnostics unavailable ({bulk_error}; {error})")
    try:
        if bulk_error is None:
            vendor_report = read_bulk_report(b"P2DV", args.timeout)
        else:
            vendor_report = read_hid_report(b"P2DV")
        print_vendor_report(vendor_report, args.raw)
    except USB_EXCEPTIONS as error:
        print(f"\nSteam vendor diagnostics unavailable: {error}")
    try:
        if bulk_error is None:
            rumble_report = read_bulk_report(b"P2DR", args.timeout)
        else:
            rumble_report = read_hid_report(b"P2DR")
        print_rumble_report(rumble_report, args.raw)
    except USB_EXCEPTIONS as error:
        print(f"\nRumble diagnostics unavailable: {error}")
    if args.ble_rate > 0:
        if bulk_error is None:
            rate_reader = lambda: read_bulk_report(b"P2DG", args.timeout)
        else:
            rate_reader = lambda: read_hid_report(b"P2DG")
        try:
            monitor_ble_rate(args.ble_rate, rate_reader)
        except USB_EXCEPTIONS as error:
            print(f"BLE rate monitor failed: {error}", file=sys.stderr)
            return 1
    if args.monitor > 0:
        try:
            monitor_hid_input(args.monitor, usb_protocol_version)
        except RuntimeError as error:
            print(f"HID input monitor failed: {error}", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
