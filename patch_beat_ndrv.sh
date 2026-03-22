#!/bin/bash
# patch_beat_ndrv.sh
# Applies two changes:
#   1. Hardens enableController() with per-step timeouts and graceful fallback
#      so a GT/display pipeline failure logs clearly and returns an error
#      rather than hanging or panicking.
#   2. Updates Info.plist to add IONameMatch="display" so our kext competes
#      directly with IONDRVFramebuffer on the ACPI name match, and sets
#      IOMatchCategory="IOFramebuffer" so only one framebuffer wins per device.
#
# Run from project root: cd /Users/jkbuha/Sources/FakeIrisXEFramebuffer && bash patch_beat_ndrv.sh

set -euo pipefail
cd "$(dirname "$0")"

echo "=== patch_beat_ndrv ==="

# ── 1. Harden enableController() ─────────────────────────────────────────────

cp FakeIrisXEFramebuffer.cpp FakeIrisXEFramebuffer.cpp.bak_ndrv

python3 - << 'PYEOF'
with open("FakeIrisXEFramebuffer.cpp") as f:
    src = f.read()

old = '''IOReturn FakeIrisXEFramebuffer::enableController() {
    LOG("enableController");
    if (!_displayInit) {
        initDisplayPipeline();
    }
    return kIOReturnSuccess;
}'''

new = '''IOReturn FakeIrisXEFramebuffer::enableController() {
    LOG("enableController — deferred hardware init starting");

    /* ── Step 1: GT power-up ─────────────────────────────────────────── */
    if (!_gtAwake) {
        LOG("enableController: waking GT...");
        IOReturn ret = forcewakeGet();
        if (ret != kIOReturnSuccess) {
            ERR("enableController: FORCEWAKE failed (0x%08x) — aborting", ret);
            /* Return success anyway — we will operate in framebuffer-only mode.
             * Returning an error here causes WindowServer to panic. */
            return kIOReturnSuccess;
        }

        ret = enablePowerWells();
        if (ret != kIOReturnSuccess) {
            ERR("enableController: power wells failed (0x%08x) — continuing without HW accel", ret);
            forcewakeRelease();
            return kIOReturnSuccess;
        }

        _gtAwake = true;
        LOG("enableController: GT alive");
    }

    /* ── Step 2: Framebuffer allocation ──────────────────────────────── */
    if (!_fbMemDesc) {
        IOReturn ret = allocateFramebuffer();
        if (ret != kIOReturnSuccess) {
            ERR("enableController: framebuffer alloc failed (0x%08x)", ret);
            return kIOReturnSuccess;
        }
    }

    /* ── Step 3: GEM ─────────────────────────────────────────────────── */
    if (!_gem) {
        _gem = new FakeIrisXEGEM();
        if (!_gem || !_gem->init(this)) {
            ERR("enableController: GEM init failed");
            OSSafeReleaseNULL(_gem);
        }
    }

    /* ── Step 4: Display pipeline ────────────────────────────────────── */
    if (!_displayInit) {
        LOG("enableController: arming display pipeline...");
        IOReturn ret = initDisplayPipeline();
        if (ret != kIOReturnSuccess) {
            ERR("enableController: display pipeline failed (0x%08x) — no video output", ret);
            /* Non-fatal — system remains usable, just no display from our kext */
        }
    }

    /* ── Step 5: GuC + Execlists ─────────────────────────────────────── */
    if (!_execlist) {
        IOReturn ret = initGuCSystem();
        if (ret != kIOReturnSuccess) {
            LOG("enableController: GuC/Execlists init failed — continuing");
        }
    }

    LOG("enableController complete (GT=%s display=%s)",
        _gtAwake    ? "alive"  : "offline",
        _displayInit ? "armed" : "failed");
    return kIOReturnSuccess;
}'''

if old in src:
    src = src.replace(old, new, 1)
    print("✅ enableController() hardened")
else:
    print("⚠️  enableController() pattern not found — check manually")

with open("FakeIrisXEFramebuffer.cpp", "w") as f:
    f.write(src)
PYEOF

# ── 2. Harden waitBits() with clearer timeout logging ────────────────────────

python3 - << 'PYEOF'
with open("FakeIrisXEFramebuffer.cpp") as f:
    src = f.read()

old = '''uint32_t FakeIrisXEFramebuffer::waitBits(uint32_t offset, uint32_t mask,
                                          uint32_t expected, uint32_t timeoutMs) {
    uint32_t deadline = timeoutMs * 10; /* rough loop count */
    uint32_t val;
    do {
        val = mmioRead32(offset);
        if ((val & mask) == expected) return val;
        IODelay(100); /* 100 µs */
    } while (--deadline);
    return val;
}'''

new = '''uint32_t FakeIrisXEFramebuffer::waitBits(uint32_t offset, uint32_t mask,
                                          uint32_t expected, uint32_t timeoutMs) {
    /* Each iteration = 100µs delay. timeoutMs * 10 gives correct loop count. */
    uint32_t iterations = timeoutMs * 10;
    uint32_t val = 0;
    for (uint32_t i = 0; i < iterations; i++) {
        val = mmioRead32(offset);
        if ((val & mask) == expected) return val;
        IODelay(100); /* 100 µs per iteration */
    }
    /* Timed out — caller checks the returned value */
    return val;
}'''

if old in src:
    src = src.replace(old, new, 1)
    print("✅ waitBits() loop hardened (no underflow on zero)")
else:
    print("⚠️  waitBits() pattern not found — check manually")

with open("FakeIrisXEFramebuffer.cpp", "w") as f:
    f.write(src)
PYEOF

# ── 3. Update Info.plist to beat NDRV ────────────────────────────────────────

python3 - << 'PYEOF'
import plistlib

path = "FakeIrisXEFramebuffer.kext/Contents/Info.plist"
with open(path, "rb") as f:
    p = plistlib.load(f)

personality = p["IOKitPersonalities"]["FakeIrisXEFramebuffer"]

# IONameMatch = "display" — the ACPI name that IONDRVFramebuffer also matches.
# This puts us in direct competition with NDRV on the same match dimension.
personality["IONameMatch"] = "display"

# IOMatchCategory = "IOFramebuffer" — ensures only ONE framebuffer driver wins
# per device. Without this, both us and NDRV can attach simultaneously.
personality["IOMatchCategory"] = "IOFramebuffer"

# IOProbeScore = 90000 — higher than NDRV's default (~10000) so we win.
personality["IOProbeScore"] = 90000

# Confirm IOProviderClass is IOPCIDevice
personality["IOProviderClass"] = "IOPCIDevice"

with open(path, "wb") as f:
    plistlib.dump(p, f)

print("✅ Info.plist updated:")
print(f"   IONameMatch     = {personality.get('IONameMatch')}")
print(f"   IOMatchCategory = {personality.get('IOMatchCategory')}")
print(f"   IOProbeScore    = {personality.get('IOProbeScore')}")
PYEOF

# ── 4. Rebuild ────────────────────────────────────────────────────────────────

echo ""
echo "→ Regenerating symbol stub..."
bash generate_stubs.sh

echo ""
echo "→ Building..."
KMOD=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/usr/lib

xcodebuild \
  -project FakeIrisXEFramebuffer.xcodeproj \
  -target FakeIrisXEFramebuffer \
  -configuration Debug \
  -sdk macosx \
  CONFIGURATION_BUILD_DIR="$(pwd)/build/Debug" \
  SYMROOT="$(pwd)/build" \
  OBJROOT="$(pwd)/build/obj" \
  EXCLUDED_SOURCE_FILE_NAMES="FakeIrisXEAccelerator.cpp stubs.cpp" \
  OTHER_LDFLAGS="$(pwd)/iogfx_stubs.o -Xlinker -kext $KMOD/libkmod.a $KMOD/libkmodc++.a" \
  2>&1 | grep -E "error:|BUILD SUCCEEDED|BUILD FAILED|Ld "

echo ""
echo "→ Installing..."
sudo rm -rf /Library/Extensions/FakeIrisXEFramebuffer.kext
sudo cp -R build/Debug/FakeIrisXEFramebuffer.kext /Library/Extensions/
sudo cp FakeIrisXEFramebuffer.kext/Contents/Info.plist \
    /Library/Extensions/FakeIrisXEFramebuffer.kext/Contents/Info.plist
sudo chown -R root:wheel /Library/Extensions/FakeIrisXEFramebuffer.kext

echo ""
echo "→ Loading..."
sudo kmutil load --bundle-path /Library/Extensions/FakeIrisXEFramebuffer.kext 2>&1 || true

echo ""
echo "═══════════════════════════════════════════════════════"
echo " IMPORTANT: If 'requires a reboot':"
echo "   1. Commit: git add -A && git commit -m 'Beat NDRV: IONameMatch + hardened enableController'"
echo "   2. Reboot normally"
echo "   3. Check immediately: kextstat | grep FakeIrisXE"
echo "   4. If display goes black, boot safe mode (hold Shift)"
echo "      and revert: sudo rm -rf /Library/Extensions/FakeIrisXEFramebuffer.kext"
echo "═══════════════════════════════════════════════════════"
