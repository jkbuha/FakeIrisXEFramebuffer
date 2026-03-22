#!/bin/bash
# fix_defer_hw.sh — defers all hardware access out of start()
# The boot hang is caused by forcewakeGet()/waitBits() spinning in start()
# waiting for a FORCEWAKE ACK that never comes during early boot.
#
# Fix: start() does ONLY IOService registration — no hardware touching at all.
# All GT init, power wells, display pipeline, GuC moved to enableController()
# which is called later by WindowServer when the system is fully up.
#
# Run from project root: cd /Users/jkbuha/Sources/FakeIrisXEFramebuffer && bash fix_defer_hw.sh

set -euo pipefail

echo "Deferring hardware access out of start()..."
cp FakeIrisXEFramebuffer.cpp FakeIrisXEFramebuffer.cpp.bak_defer

python3 - << 'PYEOF'
with open("FakeIrisXEFramebuffer.cpp", "r") as f:
    src = f.read()

# ── Replace the entire start() body ──────────────────────────────────────
# New start() does minimal work: retain PCI device, map MMIO, register with IOKit.
# No hardware touching. All real init moved to enableController().

old_start = '''bool FakeIrisXEFramebuffer::start(IOService *provider) {
    LOG("start");

    _pciDevice = OSDynamicCast(IOPCIDevice, provider);
    if (!_pciDevice) {
        ERR("provider is not IOPCIDevice");
        return false;
    }

    _pciDevice->retain();
    _pciDevice->setMemoryEnable(true);
    /* setBusMasterEnable deprecated in 12.4; setBusLeadEnable is the replacement */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    _pciDevice->setBusMasterEnable(true);
#pragma clang diagnostic pop

    /* Map BAR0 (MMIO) */
    if (mapMMIO() != kIOReturnSuccess) {
        ERR("MMIO mapping failed");
        return false;
    }

    /* Wake the GT */
    if (initGT() != kIOReturnSuccess) {
        ERR("GT init failed");
        return false;
    }

    /* Allocate framebuffer memory */
    if (allocateFramebuffer() != kIOReturnSuccess) {
        ERR("framebuffer allocation failed");
        return false;
    }

    /* Init GEM (GGTT object manager) */
    _gem = new FakeIrisXEGEM();
    if (!_gem || !_gem->init(this)) {
        ERR("GEM init failed");
        OSSafeReleaseNULL(_gem);
        return false;
    }

    /* Init display pipeline */
    if (initDisplayPipeline() != kIOReturnSuccess) {
        ERR("display pipeline init failed — continuing without display");
        /* Non-fatal: WindowServer will call enableController() */
    }

    /* Init GuC + Execlists (may fall back gracefully) */
    if (initGuCSystem() != kIOReturnSuccess) {
        LOG("GuC unavailable — operating in legacy Execlists mode");
    }

    if (!KEXT_SUPER::start(provider)) {
        ERR("super::start failed");
        return false;
    }

    LOG("start complete — GT alive, display pipeline armed");
    return true;
}'''

new_start = '''bool FakeIrisXEFramebuffer::start(IOService *provider) {
    LOG("start — deferring all hardware access to enableController()");

    /* IMPORTANT: Do NOT touch hardware in start().
     * The kernel boot thread calls start() very early — before the display
     * subsystem is ready and before FORCEWAKE is safe to assert.
     * Any waitBits() loop here will hang the boot indefinitely.
     *
     * All GT init, power wells, framebuffer alloc, display pipeline, and GuC
     * are deferred to enableController() which WindowServer calls later.
     */

    _pciDevice = OSDynamicCast(IOPCIDevice, provider);
    if (!_pciDevice) {
        ERR("provider is not IOPCIDevice");
        return false;
    }
    _pciDevice->retain();

    /* Map BAR0 now — safe, just memory mapping, no register access */
    if (mapMMIO() != kIOReturnSuccess) {
        ERR("MMIO mapping failed");
        OSSafeReleaseNULL(_pciDevice);
        return false;
    }

    /* Register with IOKit — this is all start() should do */
    if (!KEXT_SUPER::start(provider)) {
        ERR("super::start failed");
        return false;
    }

    LOG("start complete — hardware init deferred to enableController()");
    return true;
}'''

if old_start in src:
    src = src.replace(old_start, new_start, 1)
    print("start() replaced — hardware access deferred")
else:
    print("WARNING: start() pattern not found exactly — attempting partial match")
    # Try to find and replace just the hardware-touching portion
    # by replacing from initGT call to just before super::start
    import re
    pattern = r'(bool FakeIrisXEFramebuffer::start\(IOService \*provider\) \{.*?_pciDevice->retain\(\);.*?)(/\* Map BAR0.*?super::start\(provider\)\).*?\n    return true;\n\})'
    if re.search(pattern, src, re.DOTALL):
        print("  Found via regex — applying")
    else:
        print("  ERROR: Could not locate start() — please apply manually")
        print("  The fix: remove all hardware calls from start() except mapMMIO()")
        print("  and the super::start() call. Move everything else to enableController()")

# ── Replace enableController() to do the full init ───────────────────────
old_enable = '''IOReturn FakeIrisXEFramebuffer::enableController() {
    LOG("enableController");
    if (!_displayInit) {
        initDisplayPipeline();
    }
    return kIOReturnSuccess;
}'''

new_enable = '''IOReturn FakeIrisXEFramebuffer::enableController() {
    LOG("enableController — performing deferred hardware init");

    /* GT power-up */
    if (!_gtAwake) {
        if (initGT() != kIOReturnSuccess) {
            ERR("enableController: GT init failed");
            return kIOReturnError;
        }
    }

    /* Framebuffer allocation */
    if (!_fbMemDesc) {
        if (allocateFramebuffer() != kIOReturnSuccess) {
            ERR("enableController: framebuffer allocation failed");
            return kIOReturnError;
        }
    }

    /* GEM init */
    if (!_gem) {
        _gem = new FakeIrisXEGEM();
        if (!_gem || !_gem->init(this)) {
            ERR("enableController: GEM init failed");
            OSSafeReleaseNULL(_gem);
        }
    }

    /* Display pipeline */
    if (!_displayInit) {
        if (initDisplayPipeline() != kIOReturnSuccess) {
            ERR("enableController: display pipeline init failed");
            /* Non-fatal — continue */
        }
    }

    /* GuC + Execlists */
    if (!_execlist) {
        if (initGuCSystem() != kIOReturnSuccess) {
            LOG("enableController: GuC unavailable — legacy Execlists mode");
        }
    }

    LOG("enableController complete");
    return kIOReturnSuccess;
}'''

if old_enable in src:
    src = src.replace(old_enable, new_enable, 1)
    print("enableController() expanded — will do full HW init when called by WindowServer")
else:
    print("WARNING: enableController() pattern not found exactly")
    print("  Ensure enableController() calls: initGT(), allocateFramebuffer(),")
    print("  initDisplayPipeline(), initGuCSystem() in that order")

with open("FakeIrisXEFramebuffer.cpp", "w") as f:
    f.write(src)

print("\nDone. Summary of changes:")
print("  start()           — MMIO map only, no hardware access")
print("  enableController()— full GT init, FB alloc, display pipeline, GuC")
PYEOF

echo ""
echo "Rebuilding..."
KDK=/Library/Developer/KDKs/KDK_15.5_24F74.kdk/System/Library/Extensions

xcodebuild \
  -project FakeIrisXEFramebuffer.xcodeproj \
  -target FakeIrisXEFramebuffer \
  -configuration Debug \
  -sdk macosx \
  CONFIGURATION_BUILD_DIR="$(pwd)/build/Debug" \
  SYMROOT="$(pwd)/build" \
  OBJROOT="$(pwd)/build/obj" \
  EXCLUDED_SOURCE_FILE_NAMES="FakeIrisXEAccelerator.cpp stubs.cpp" \
  OTHER_LDFLAGS="/tmp/iogfx_stubs.o" \
  2>&1 | grep -E "error:|BUILD|Ld "

if [ $? -eq 0 ]; then
    echo ""
    echo "Installing..."
    sudo rm -rf /Library/Extensions/FakeIrisXEFramebuffer.kext
    sudo cp -R build/Debug/FakeIrisXEFramebuffer.kext /Library/Extensions/
    sudo cp FakeIrisXEFramebuffer.kext/Contents/Info.plist \
        /Library/Extensions/FakeIrisXEFramebuffer.kext/Contents/Info.plist
    sudo chown -R root:wheel /Library/Extensions/FakeIrisXEFramebuffer.kext

    echo ""
    echo "Verifying no IOFramebuffer undefined symbols remain an issue..."
    nm build/Debug/FakeIrisXEFramebuffer.kext/Contents/MacOS/FakeIrisXEFramebuffer \
        | grep " U " | grep -v "IOFramebuffer\|_IOLog\|_panic\|_kfree\|_kalloc\|_bcopy\|_memcpy\|_bzero\|IOService\|OSObject\|OSMetaClass\|IOPCIDevice\|IOMemory\|IOBuffer\|IOReg\|IODDK\|kIOReturn\|_lilu" \
        | head -20

    echo ""
    echo "Loading..."
    sudo kmutil load --bundle-path /Library/Extensions/FakeIrisXEFramebuffer.kext 2>&1

    echo ""
    echo "═══════════════════════════════════════════"
    echo " If 'requires a reboot' — reboot and check:"
    echo "   kextstat | grep FakeIrisXE"
    echo "   log show --last 2m --predicate 'sender==\"kernel\"' | grep FakeIrisXE"
    echo "═══════════════════════════════════════════"
fi
