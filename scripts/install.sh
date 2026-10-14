#!/usr/bin/env bash
set -euo pipefail

BITWIG_LIB_DIR="${BITWIG_LIB_DIR:-/opt/bitwig-studio/lib/bitwig-studio}"
TARGET_SO="${BITWIG_LIB_DIR}/libbitwig-jni.so"
BACKUP_SO="${BITWIG_LIB_DIR}/libbitwig-jni.so.bak"
SHIM_SO="${BITWIG_LIB_DIR}/librekordbox_shim.so"

if [[ $EUID -ne 0 ]]; then
   echo "Error: This script must be run as root (use sudo)." >&2
   exit 1
fi

if [[ ! -f "${TARGET_SO}" ]]; then
    echo "Error: ${TARGET_SO} not found." >&2
    exit 1
fi

command -v patchelf >/dev/null 2>&1 || { echo "patchelf is required. Run ./scripts/check_requirements.sh first."; exit 1; }

echo "==> Building shim..."
make -B

if [[ ! -f "${BACKUP_SO}" ]]; then
    echo "==> Creating backup: ${BACKUP_SO}"
    cp "${TARGET_SO}" "${BACKUP_SO}"
else
    echo "==> Backup already exists: ${BACKUP_SO}"
fi

echo "==> Copying shim to ${BITWIG_LIB_DIR}..."
cp build/librekordbox_shim.so "${SHIM_SO}"
chmod 755 "${SHIM_SO}"

echo "==> Patching symbols in ${TARGET_SO}..."
python3 scripts/patch_symbols.py "${TARGET_SO}"

echo "==> Updating dynamic linker tags..."
patchelf --add-needed librekordbox_shim.so "${TARGET_SO}" || true
patchelf --set-rpath '$ORIGIN' "${TARGET_SO}"

# Create dummy sample for test track loading
touch /tmp/test.wav

echo "==> Done! Launch Bitwig Studio and right-click Browser -> 'Restore Quick Sources to defaults' if needed."