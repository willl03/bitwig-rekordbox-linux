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

if [[ -f "${BACKUP_SO}" ]]; then
    echo "==> Restoring pristine backup: ${BACKUP_SO} -> ${TARGET_SO}"
    cp "${BACKUP_SO}" "${TARGET_SO}"
    rm -f "${BACKUP_SO}"
else
    echo "==> No backup file found. Reverting symbols in-place..."
    python3 -c '
from pathlib import Path
target = Path("'"${TARGET_SO}"'")
data = bytearray(target.read_bytes())
symbols = [
    b"Xava_com_bitwig_base_rekordbox_library_RekordboxSDK_initNative",
    b"Xava_com_bitwig_base_rekordbox_library_RekordboxSDK_deinitNative",
    b"Xava_com_bitwig_base_rekordbox_library_RekordboxSDK_dump",
    b"Xava_com_bitwig_base_rekordbox_library_RekordboxSDK_getSongs",
    b"Xava_com_bitwig_base_rekordbox_library_RekordboxSDK_getRawPlaylists",
    b"Xava_com_bitwig_base_rekordbox_library_RekordboxSDK_getSongIDsInPlaylist",
    b"Xava_com_bitwig_base_rekordbox_library_RekordboxSDK_getSongDetails",
]
for s in symbols:
    rep = b"J" + s[1:]
    pos = 0
    while True:
        pos = data.find(s, pos)
        if pos == -1: break
        data[pos:pos+len(s)] = rep
        pos += len(rep)
target.write_bytes(data)
'
    patchelf --remove-needed librekordbox_shim.so "${TARGET_SO}" 2>/dev/null || true
fi

if [[ -f "${SHIM_SO}" ]]; then
    echo "==> Removing ${SHIM_SO}..."
    rm -f "${SHIM_SO}"
fi

echo "==> Uninstallation clean and complete."