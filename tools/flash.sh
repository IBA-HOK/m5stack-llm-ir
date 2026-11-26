#!/usr/bin/env bash
# Build and upload the m5stack-llm-ir firmware.
#
# Usage:
#   tools/flash.sh                    # CoreS3, stub IR (Unit IR not connected yet)
#   tools/flash.sh m5stack-cores3-ir  # CoreS3 + Unit IR enabled
#   tools/flash.sh m5stack-core2-ir   # any env from firmware/platformio.ini
#
# Wi-Fi / API / relay settings are read from the git-ignored `.env` file at the
# repository root (see `.env.example`). The script refuses to run when no USB
# serial device is present or when the serial device is not readable by the
# current user (Arch Linux: add yourself to the `uucp` group).
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENV_NAME="${1:-m5stack-cores3}"
PIO="${ROOT_DIR}/.venv/bin/pio"
ENV_FILE="${ROOT_DIR}/.env"

if [[ ! -x "${PIO}" ]]; then
  echo "error: PlatformIO not found at ${PIO}" >&2
  echo "       run: python3 -m venv .venv && .venv/bin/pip install platformio" >&2
  exit 1
fi

if [[ ! -f "${ENV_FILE}" ]]; then
  echo "warning: ${ENV_FILE} is missing (Wi-Fi/API key/relay stay disabled)." >&2
  echo "         cp .env.example .env  # then edit it" >&2
elif ! grep -qE '^[[:space:]]*WIFI_SSID=[^[:space:]]' "${ENV_FILE}"; then
  echo "warning: WIFI_SSID in .env is empty; the device will not join the network." >&2
fi

mapfile -t PORTS < <(ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null || true)
if [[ ${#PORTS[@]} -eq 0 ]]; then
  echo "error: no USB serial device found (/dev/ttyACM* or /dev/ttyUSB*)." >&2
  echo "       - use a data-capable USB cable (not charge-only)" >&2
  echo "       - CoreS3/AtomS3 show up as /dev/ttyACM0; Core2/StickC Plus2 as /dev/ttyUSB0 or /dev/ttyACM0" >&2
  echo "       - check: lsusb | grep -Ei '303a|10c4|1a86'" >&2
  exit 1
fi

for port in "${PORTS[@]}"; do
  if [[ ! -r "${port}" || ! -w "${port}" ]]; then
    echo "error: ${port} is not readable/writable by $USER." >&2
    echo "       Arch Linux (serial group is 'uucp'):" >&2
    echo "         sudo usermod -aG uucp \"\$USER\"   # log in again afterwards" >&2
    echo "       or for a one-off test:" >&2
    echo "         sudo chmod a+rw ${port}" >&2
    exit 1
  fi
done

echo "detected serial port(s): ${PORTS[*]}"
echo "building env '${ENV_NAME}' (secrets injected from .env) and uploading..."
"${PIO}" run --project-dir "${ROOT_DIR}/firmware" -e "${ENV_NAME}" -t upload "$@"
echo "done. Serial log:"
echo "  ${PIO} device monitor --project-dir ${ROOT_DIR}/firmware -e ${ENV_NAME}"
