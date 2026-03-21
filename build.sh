#!/bin/bash
# build.sh — FakeIrisXEFramebuffer.kext build script
# Run from the project root directory (where this script lives).
# Requires: Xcode command line tools, macOS Sequoia 15 SDK
#
# Usage:
#   ./build.sh          — Debug build
#   ./build.sh release  — Release build
#   ./build.sh clean    — Clean derived data
#   ./build.sh install  — Build + copy to EFI/OC/Kexts (set EFI_MOUNT below)
#   ./build.sh load     — Build + kextload for live testing (needs SIP disabled)
#
# ── Boot args required in OpenCore config.plist ─────────────────────────
#   amfi_get_out_of_my_way=1
#   -lilubetaall
#   -wegbeta
#   keepsyms=1
#   debug=0x100
# ────────────────────────────────────────────────────────────────────────

set -euo pipefail

# ── Config ───────────────────────────────────────────────────────────────
PROJECT_NAME="FakeIrisXEFramebuffer"
KEXT_NAME="${PROJECT_NAME}.kext"
BUILD_DIR="$(pwd)/build"
EFI_MOUNT="/Volumes/EFI"   # ← change to your EFI partition mount point

CONFIGURATION="${1:-Debug}"
case "$CONFIGURATION" in
    release|Release) CONFIGURATION="Release" ;;
    clean|Clean)
        echo "── Cleaning ──────────────────────────────────"
        rm -rf "$BUILD_DIR"
        echo "Done."
        exit 0
        ;;
    install|Install) CONFIGURATION="Release" ; DO_INSTALL=1 ;;
    load|Load)       CONFIGURATION="Debug"   ; DO_LOAD=1 ;;
    debug|Debug|*)   CONFIGURATION="Debug" ;;
esac

echo "══════════════════════════════════════════════════"
echo " Building ${KEXT_NAME} [${CONFIGURATION}]"
echo "══════════════════════════════════════════════════"

# ── Verify SDK ───────────────────────────────────────────────────────────
SDK_PATH=$(xcrun --show-sdk-path --sdk macosx 2>/dev/null || true)
if [[ -z "$SDK_PATH" ]]; then
    echo "ERROR: macOS SDK not found. Install Xcode command line tools:"
    echo "  xcode-select --install"
    exit 1
fi
echo "SDK: $SDK_PATH"
SDK_VER=$(xcrun --show-sdk-version --sdk macosx)
echo "SDK version: $SDK_VER"

# Warn if SDK is older than 15.0
SDK_MAJOR=$(echo "$SDK_VER" | cut -d. -f1)
if [[ "$SDK_MAJOR" -lt 15 ]]; then
    echo "WARNING: SDK $SDK_VER is older than Sequoia 15.0"
    echo "         KPI version mismatch may prevent loading on Sequoia."
fi

# ── Build ─────────────────────────────────────────────────────────────────
mkdir -p "$BUILD_DIR"

xcodebuild \
    -project "${PROJECT_NAME}.xcodeproj" \
    -target  "${PROJECT_NAME}" \
    -configuration "$CONFIGURATION" \
    -sdk macosx \
    CONFIGURATION_BUILD_DIR="$BUILD_DIR/$CONFIGURATION" \
    SYMROOT="$BUILD_DIR" \
    OBJROOT="$BUILD_DIR/obj" \
    | tee "$BUILD_DIR/build.log"

KEXT_PATH="$BUILD_DIR/$CONFIGURATION/$KEXT_NAME"

if [[ ! -d "$KEXT_PATH" ]]; then
    echo ""
    echo "ERROR: Build product not found at: $KEXT_PATH"
    echo "       Check $BUILD_DIR/build.log for errors."
    exit 1
fi

echo ""
echo "══════════════════════════════════════════════════"
echo " Build succeeded: $KEXT_PATH"
echo "══════════════════════════════════════════════════"

# ── Verify kext structure ────────────────────────────────────────────────
echo ""
echo "── Kext verification ─────────────────────────────"

# Check bundle structure
for required in \
    "Contents/Info.plist" \
    "Contents/MacOS/${PROJECT_NAME}"
do
    if [[ -e "$KEXT_PATH/$required" ]]; then
        echo "  ✓ $required"
    else
        echo "  ✗ MISSING: $required"
    fi
done

# Check binary architecture
BINARY="$KEXT_PATH/Contents/MacOS/$PROJECT_NAME"
if [[ -f "$BINARY" ]]; then
    ARCH=$(lipo -archs "$BINARY" 2>/dev/null || echo "unknown")
    echo "  Architecture: $ARCH"
    FILE_TYPE=$(file "$BINARY" | grep -o "Mach-O.*" || echo "unknown")
    echo "  File type: $FILE_TYPE"
fi

# Check Info.plist is parseable
if plutil -lint "$KEXT_PATH/Contents/Info.plist" > /dev/null 2>&1; then
    echo "  ✓ Info.plist valid"
else
    echo "  ✗ Info.plist has errors:"
    plutil -lint "$KEXT_PATH/Contents/Info.plist"
fi

# kextutil static check (will warn about missing code signing — expected)
echo ""
echo "── kextutil static check ─────────────────────────"
kextutil -n -print-diagnostics "$KEXT_PATH" 2>&1 || true
echo "  (Code signing warnings expected — SIP must be disabled to load)"

# ── Install to EFI (OpenCore injection) ──────────────────────────────────
if [[ "${DO_INSTALL:-0}" == "1" ]]; then
    echo ""
    echo "── Installing to OpenCore EFI ─────────────────────"
    EFI_KEXTS="${EFI_MOUNT}/EFI/OC/Kexts"
    if [[ ! -d "$EFI_KEXTS" ]]; then
        echo "ERROR: EFI Kexts directory not found: $EFI_KEXTS"
        echo "       Mount your EFI partition and set EFI_MOUNT in this script."
        exit 1
    fi
    rm -rf "${EFI_KEXTS}/${KEXT_NAME}"
    cp -R "$KEXT_PATH" "$EFI_KEXTS/"
    echo "  ✓ Copied to $EFI_KEXTS/$KEXT_NAME"
    echo ""
    echo "  REMEMBER: Add to config.plist → Kernel → Add:"
    echo "    BundlePath:     ${KEXT_NAME}"
    echo "    ExecutablePath: Contents/MacOS/${PROJECT_NAME}"
    echo "    PlistPath:      Contents/Info.plist"
    echo "    MinKernel:      24.0.0"
    echo "    Enabled:        true"
fi

# ── kextload for live iteration ───────────────────────────────────────────
if [[ "${DO_LOAD:-0}" == "1" ]]; then
    echo ""
    echo "── Loading kext for live test ─────────────────────"
    echo "  WARNING: Requires SIP disabled and amfi_get_out_of_my_way=1"

    INSTALL_PATH="/Library/Extensions/${KEXT_NAME}"
    sudo rm -rf "$INSTALL_PATH"
    sudo cp -R "$KEXT_PATH" "$INSTALL_PATH"
    sudo chown -R root:wheel "$INSTALL_PATH"
    sudo chmod -R 755 "$INSTALL_PATH"
    sudo kextcache -i / 2>/dev/null || true

    echo "  Loading..."
    sudo kextload "$INSTALL_PATH" && {
        echo "  ✓ Loaded"
        echo ""
        echo "  Status:"
        kextstat | grep FakeIrisXE || echo "  (not visible in kextstat — may need reboot)"
        echo ""
        echo "  Live log (ctrl-C to stop):"
        echo "  log stream --predicate 'sender == \"kernel\"' --level debug | grep -E 'FakeIrisXE|GuC|Execlist|FORCEWAKE'"
    } || {
        echo "  ✗ kextload failed"
        echo "  Check: sudo dmesg | grep -E 'FakeIrisXE|kext'"
    }
fi

echo ""
echo "Done."
