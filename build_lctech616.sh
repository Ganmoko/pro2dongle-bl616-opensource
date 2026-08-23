#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "$0")" && pwd)"
export BL_SDK_BASE="${BL_SDK_BASE:-$project_dir/../bouffalo_sdk}"
toolchain_bin="$project_dir/../toolchain_gcc_t-head_linux/bin"
usb_speed="${USB_SPEED:-fs}"

case "$usb_speed" in
    fs)
        export FORCE_FS=1
        ;;
    hs)
        unset FORCE_FS || true
        ;;
    *)
        echo "Unsupported USB_SPEED=$usb_speed (use fs or hs)." >&2
        exit 1
        ;;
esac

if ! command -v riscv64-unknown-elf-gcc >/dev/null 2>&1 &&
   [[ -x "$toolchain_bin/riscv64-unknown-elf-gcc" ]]; then
    export PATH="$toolchain_bin:$PATH"
fi

if [[ ! -f "$BL_SDK_BASE/project.build" ]]; then
    echo "Bouffalo SDK not found: $BL_SDK_BASE" >&2
    echo "Clone sqlCRT/bouffalo_sdk beside this directory or set BL_SDK_BASE." >&2
    exit 1
fi

if ! command -v riscv64-unknown-elf-gcc >/dev/null 2>&1; then
    echo "riscv64-unknown-elf-gcc not found in PATH." >&2
    echo "Install bouffalolab/toolchain_gcc_t-head_linux beside this project." >&2
    exit 1
fi

echo "[build] Target: LCTech BL616; USB: $usb_speed"
make -C "$project_dir" CHIP=bl616 BOARD=bl616dk "$@"
