# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Planned
- Pioneer `ANLZ0000.EXT` (`PSSI`) song structure / phrase slicing support.

## [1.0.0] - 2026-10-13

### Added
- Added support for Memory Cues. They're mapped to letters I-P without color to differentiate them from Hot Cues.

## [1.0.0] - 2026-10-13

### Added
- Initial release of the native C/JNI Rekordbox bridge (`librekordbox_shim.so`) for Bitwig Studio on Linux (tested on Bitwig 6.1.4+).
- Direct SQLCipher decryption of Rekordbox 6 and 7 `master.db` databases in read-only mode (`SQLITE_OPEN_READONLY`).
- Full playlist, crate, and nested folder hierarchy reconstruction matching Rekordbox sequencing (`Seq`).
- Native binary parser for Pioneer `ANLZ0000.DAT` (`PQTZ` chunks) to extract accurate beatgrids, tempo changes, and downbeat offsets without triggering Bitwig's CPU transient analysis.
- Synthetic beatgrid fallback generator when `ANLZ` files are missing.
- Hot Cue (A–H), Memory Cue, and Memory Loop extraction (`djmdCue`) with millisecond accuracy.
- Automated dependency checker (`check_requirements.sh`), installer (`install.sh`), and uninstaller (`uninstall.sh`) with automatic `libbitwig-jni.so.bak` backup and restoration.