#!/usr/bin/env python3
import sys
from pathlib import Path

TARGET_SYMBOLS = [
    b"Java_com_bitwig_base_rekordbox_library_RekordboxSDK_initNative",
    b"Java_com_bitwig_base_rekordbox_library_RekordboxSDK_deinitNative",
    b"Java_com_bitwig_base_rekordbox_library_RekordboxSDK_dump",
    b"Java_com_bitwig_base_rekordbox_library_RekordboxSDK_getSongs",
    b"Java_com_bitwig_base_rekordbox_library_RekordboxSDK_getRawPlaylists",
    b"Java_com_bitwig_base_rekordbox_library_RekordboxSDK_getSongIDsInPlaylist",
    b"Java_com_bitwig_base_rekordbox_library_RekordboxSDK_getSongDetails",
]

def patch_file(file_path: Path):
    data = bytearray(file_path.read_bytes())
    modified = 0

    for sym in TARGET_SYMBOLS:
        replacement = b"X" + sym[1:]
        pos = 0
        while True:
            pos = data.find(sym, pos)
            if pos == -1:
                break
            data[pos:pos + len(sym)] = replacement
            print(f"Patched: {sym.decode()} -> {replacement.decode()} at {hex(pos)}")
            pos += len(replacement)
            modified += 1

    if modified > 0:
        file_path.write_bytes(data)
        print(f"Successfully mutated {modified} symbols in {file_path}")
    else:
        print("No target symbols found to mutate (may already be patched).")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: patch_symbols.py <path_to_libbitwig-jni.so>")
        sys.exit(1)
    patch_file(Path(sys.argv[1]))