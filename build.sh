#!/bin/bash
# build.sh — FakeIrisXEFramebuffer build + install + load
# Usage:
#   bash build.sh          # build only
#   bash build.sh install  # build + install to /Library/Extensions
#   bash build.sh load     # build + install + load (requires prior approval)
#
# Prerequisites:
#   - KDK 15.5 (24F74) installed to /Library/Developer/KDKs/
#   - SIP disabled: csrutil disable + csrutil authenticated-root disable
#   - amfi_get_out_of_my_way=1 in OpenCore boot-args

set -euo pipefail
cd "$(dirname "$0")"

KMOD=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/usr/lib
BUILD_DIR="$(pwd)/build/Debug"
KEXT="FakeIrisXEFramebuffer.kext"
INSTALL_PATH="/Library/Extensions/$KEXT"

echo "=== FakeIrisXEFramebuffer ==="

# Preflight
if [[ ! -d /Library/Developer/KDKs/KDK_15.5_24F74.kdk ]]; then
    echo "ERROR: KDK 15.5 (24F74) not found."
    echo "       Download: https://github.com/dortania/KdkSupportPkg/releases/tag/24F74"
    exit 1
fi

# Stub
echo "→ Generating symbol stub..."
bash generate_stubs.sh

# Build
echo "→ Building..."
xcodebuild \
  -project FakeIrisXEFramebuffer.xcodeproj \
  -target FakeIrisXEFramebuffer \
  -configuration Debug \
  -sdk macosx \
  CONFIGURATION_BUILD_DIR="$BUILD_DIR" \
  SYMROOT="$(pwd)/build" \
  OBJROOT="$(pwd)/build/obj" \
  EXCLUDED_SOURCE_FILE_NAMES="FakeIrisXEAccelerator.cpp stubs.cpp" \
  OTHER_LDFLAGS="$(pwd)/iogfx_stubs.o -Xlinker -kext $KMOD/libkmod.a $KMOD/libkmodc++.a" \
  2>&1 | grep -E "error:|BUILD SUCCEEDED|BUILD FAILED|Ld "

echo "✅ $BUILD_DIR/$KEXT"

# Install
if [[ "${1:-}" == "install" || "${1:-}" == "load" ]]; then
    echo "→ Installing to $INSTALL_PATH ..."
    sudo rm -rf "$INSTALL_PATH"
    sudo cp -R "$BUILD_DIR/$KEXT" /Library/Extensions/
    sudo cp "FakeIrisXEFramebuffer.kext/Contents/Info.plist" "$INSTALL_PATH/Contents/Info.plist"
    sudo chown -R root:wheel "$INSTALL_PATH"
    echo "✅ Installed"
fi

# Load
if [[ "${1:-}" == "load" ]]; then
    echo "→ Loading..."
    sudo kmutil load --bundle-path "$INSTALL_PATH" 2>&1 || true
    echo "→ Status:"
    kextstat | grep FakeIrisXE || echo "  Not loaded — approve in System Settings or reboot"
fi
