#!/usr/bin/env bash
set -euo pipefail

echo "==> Checking system dependencies..."

if command -v apt-get >/dev/null 2>&1; then
    PKG_MANAGER="apt"
elif command -v dnf >/dev/null 2>&1; then
    PKG_MANAGER="dnf"
elif command -v pacman >/dev/null 2>&1; then
    PKG_MANAGER="pacman"
else
    PKG_MANAGER="unknown"
fi

install_debian() {
    echo "==> Debian/Ubuntu-based distribution detected."
    sudo apt update
    sudo apt install -y \
      build-essential \
      gcc \
      make \
      patchelf \
      python3 \
      default-jdk-headless \
      libsqlcipher-dev \
      sqlcipher
}

install_fedora() {
    echo "==> Fedora/RHEL-based distribution detected."
    sudo dnf install -y \
      gcc \
      make \
      patchelf \
      python3 \
      java-latest-openjdk-devel \
      sqlcipher-devel \
      sqlcipher
}

install_arch() {
    echo "==> Arch-based distribution detected."
    sudo pacman -Sy --noconfirm \
      base-devel \
      patchelf \
      python \
      jdk-openjdk \
      sqlcipher
}

case "$PKG_MANAGER" in
    apt)
        install_debian
        ;;
    dnf)
        install_fedora
        ;;
    pacman)
        install_arch
        ;;
    *)
        echo "Warning: Unsupported package manager. Ensure gcc, make, patchelf, python3, and JDK headers are installed."
        ;;
esac

echo ""
echo "==> Verifying tools..."
for tool in gcc make patchelf python3; do
    if command -v "$tool" >/dev/null 2>&1; then
        echo "  [OK] $tool found: $(command -v "$tool")"
    else
        echo "  [FAIL] $tool is missing!"
        exit 1
    fi
done

echo ""
echo "==> Checking JNI header files (jni.h)..."
JAVA_HOME="${JAVA_HOME:-$(dirname "$(dirname "$(readlink -f "$(which javac 2>/dev/null || which java)")")")}"

if [[ -f "${JAVA_HOME}/include/jni.h" ]]; then
    echo "  [OK] jni.h located at: ${JAVA_HOME}/include/jni.h"
else
    FOUND_JNI=$(find /usr/lib/jvm -name "jni.h" 2>/dev/null | head -n 1 || true)
    if [[ -n "$FOUND_JNI" ]]; then
        echo "  [OK] jni.h located at: ${FOUND_JNI}"
        DETECTED_DIR="$(dirname "$(dirname "$FOUND_JNI")")"
        echo "  Hint: If compilation fails, export JAVA_HOME=\"${DETECTED_DIR}\""
    else
        echo "  [FAIL] jni.h not found. Ensure an OpenJDK development package (-jdk or -devel) is installed."
        exit 1
    fi
fi

echo ""
echo "==> Checking Bitwig installation..."
BITWIG_LIB_DIR="${BITWIG_LIB_DIR:-/opt/bitwig-studio/lib/bitwig-studio}"
if [[ -f "${BITWIG_LIB_DIR}/libbitwig-jni.so" ]]; then
    echo "  [OK] libbitwig-jni.so found at: ${BITWIG_LIB_DIR}/libbitwig-jni.so"
else
    echo "  [WARN] libbitwig-jni.so not found at default path: ${BITWIG_LIB_DIR}"
    echo "         Set BITWIG_LIB_DIR if Bitwig Studio is installed elsewhere."
fi

echo ""
echo "All prerequisites satisfied!"