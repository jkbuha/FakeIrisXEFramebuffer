#!/bin/bash
# fix_all2.sh — fixes 4 remaining compile errors in FakeIrisXEFramebuffer.cpp
# Run from project root: cd /Users/jkbuha/Sources/FakeIrisXEFramebuffer && bash fix_all2.sh
set -euo pipefail

echo "══════════════════════════════════════════════════"
echo " FakeIrisXEFramebuffer — round 2 fixes"
echo "══════════════════════════════════════════════════"

if [[ ! -f "FakeIrisXEFramebuffer.cpp" ]]; then
    echo "ERROR: Run from project root directory."
    exit 1
fi

cp FakeIrisXEFramebuffer.cpp FakeIrisXEFramebuffer.cpp.bak2
echo "Backup: FakeIrisXEFramebuffer.cpp.bak2"
echo ""

python3 - << 'PYEOF'
import re

with open("FakeIrisXEFramebuffer.cpp", "r") as f:
    src = f.read()

original_len = len(src)

# ─────────────────────────────────────────────────────────────────────────
# FIX 1: Remove duplicate getApertureRange / getPixelFormats / getDisplayModeCount
# The fix_all.sh inserted a second set immediately before getDisplayModes.
# Pattern: the second set is preceded by the comment
#   "/* getDisplayModes — fills caller-allocated array with mode IDs */"
# and the first set comes earlier and is correct.
# We remove the SECOND occurrence of each (the ones inserted by fix_all.sh).
# ─────────────────────────────────────────────────────────────────────────

# Remove the duplicate block that was inserted before getDisplayModes:
# It looks like:
#   /* getDisplayModes — fills caller-allocated array with mode IDs */
#   /* getApertureRange — pure virtual. Returns IODeviceMemory for the FB aperture. */
#   IODeviceMemory * FakeIrisXEFramebuffer::getApertureRange(...) { ... }
#   /* getPixelFormats ... */
#   const char * FakeIrisXEFramebuffer::getPixelFormats(void) { ... }
#   /* getDisplayModeCount — pure virtual. */
#   IOItemCount FakeIrisXEFramebuffer::getDisplayModeCount(void) { ... }
#   IOReturn FakeIrisXEFramebuffer::getDisplayModes(...)

dup_pattern = (
    r'/\* getDisplayModes — fills caller-allocated array with mode IDs \*/\s*'
    r'/\* getApertureRange[^*]*\*/\s*'
    r'IODeviceMemory \* FakeIrisXEFramebuffer::getApertureRange\([^)]*\) \{[^}]*\}\s*'
    r'/\* getPixelFormats[^*]*\*/\s*'
    r'const char \* FakeIrisXEFramebuffer::getPixelFormats\(void\) \{[^}]*\}\s*'
    r'/\* getDisplayModeCount[^*]*\*/\s*'
    r'IOItemCount FakeIrisXEFramebuffer::getDisplayModeCount\(void\) \{\s*return 1;\s*\}\s*'
)

replacement = '/* getDisplayModes — fills caller-allocated array with mode IDs */\n'
result = re.sub(dup_pattern, replacement, src, count=1, flags=re.DOTALL)
if result == src:
    print("FIX 1: duplicate block pattern not matched — trying alternate pattern")
    # Try simpler: just find the second definition of getApertureRange and remove
    # the block from it up to (but not including) getDisplayModes
    alt_pattern = (
        r'(/\* getDisplayModes[^\n]*\n)'   # keep this comment
        r'/\* getApertureRange.*?'          # remove from here
        r'(?=IOReturn FakeIrisXEFramebuffer::getDisplayModes)'  # stop before getDisplayModes
    )
    result = re.sub(alt_pattern, r'\1', src, count=1, flags=re.DOTALL)
    if result == src:
        print("  WARNING: Could not remove duplicates automatically.")
        print("  Manually remove the second getApertureRange/getPixelFormats/getDisplayModeCount")
        print("  blocks that appear just before getDisplayModes.")
    else:
        print("FIX 1: duplicate block removed (alternate pattern)")
        src = result
else:
    print("FIX 1: duplicate getApertureRange/getPixelFormats/getDisplayModeCount removed")
    src = result

# ─────────────────────────────────────────────────────────────────────────
# FIX 2: kConnectionSupportsMonitorEdidAccess removed from Tahoe SDK
# Replace with its numeric value (0x00000406) — same constant, hardcoded.
# ─────────────────────────────────────────────────────────────────────────
old2 = '''    switch (attribute) {
        case kConnectionSupportsHLDDCSense:
        case kConnectionSupportsLLDDCSense:
        case kConnectionSupportsMonitorEdidAccess:
            *value = 1;
            return kIOReturnSuccess;
        default:
            return IOFramebuffer::getAttributeForConnection(connectIndex, attribute, value);
    }'''

new2 = '''    switch (attribute) {
        case kConnectionSupportsHLDDCSense:
        case kConnectionSupportsLLDDCSense:
        /* kConnectionSupportsMonitorEdidAccess removed from Tahoe SDK — use raw value */
        case (IOSelect)0x00000406:
            *value = 1;
            return kIOReturnSuccess;
        default:
            return IOFramebuffer::getAttributeForConnection(connectIndex, attribute, value);
    }'''

if old2 in src:
    src = src.replace(old2, new2, 1)
    print("FIX 2: kConnectionSupportsMonitorEdidAccess replaced with numeric value")
else:
    # Try without the EdidAccess line in case it was already partially fixed
    old2b = '        case kConnectionSupportsMonitorEdidAccess:'
    new2b = '        /* kConnectionSupportsMonitorEdidAccess removed from Tahoe SDK */\n        case (IOSelect)0x00000406:'
    if old2b in src:
        src = src.replace(old2b, new2b, 1)
        print("FIX 2: kConnectionSupportsMonitorEdidAccess replaced (simple form)")
    else:
        print("FIX 2: WARNING — kConnectionSupportsMonitorEdidAccess not found, may already be fixed")

# ─────────────────────────────────────────────────────────────────────────
# FIX 3: timingCEA861_1920x1080p60 — not in Tahoe SDK IOGraphicsTypes.h
# Use the raw numeric value: 0x57 (87 decimal), which is the Apple timing ID
# for CEA-861 1920x1080p60.
# ─────────────────────────────────────────────────────────────────────────
old3 = '    info->appleTimingID = timingCEA861_1920x1080p60;'
new3 = ('    /* timingCEA861_1920x1080p60 not in Tahoe SDK headers — use raw value 0x57 */\n'
        '    info->appleTimingID = (IOAppleTimingID)0x57;')

if old3 in src:
    src = src.replace(old3, new3, 1)
    print("FIX 3: timingCEA861_1920x1080p60 replaced with raw value 0x57")
else:
    print("FIX 3: timingCEA861_1920x1080p60 not found — may already be fixed")

# ─────────────────────────────────────────────────────────────────────────
# FIX 4: apertureSample field removed from IOPixelInformation in Tahoe SDK
# Simply remove that line.
# ─────────────────────────────────────────────────────────────────────────
old4 = '    pixelInfo->apertureSample    = 0;\n'
if old4 in src:
    src = src.replace(old4, '    /* apertureSample field removed from IOPixelInformation in Tahoe SDK */\n', 1)
    print("FIX 4: apertureSample assignment removed")
else:
    print("FIX 4: apertureSample not found — may already be fixed")

# ─────────────────────────────────────────────────────────────────────────
# Write result
# ─────────────────────────────────────────────────────────────────────────
with open("FakeIrisXEFramebuffer.cpp", "w") as f:
    f.write(src)

print(f"\nDone. File size: {original_len} -> {len(src)} bytes")
PYEOF

echo ""
echo "── Verification ───────────────────────────────────"
echo -n "  Duplicate getApertureRange count: "
grep -c "FakeIrisXEFramebuffer::getApertureRange" FakeIrisXEFramebuffer.cpp

echo -n "  timingCEA861 remaining: "
grep -c "timingCEA861_1920x1080p60" FakeIrisXEFramebuffer.cpp || echo "0"

echo -n "  apertureSample remaining: "
grep -c "->apertureSample" FakeIrisXEFramebuffer.cpp || echo "0"

echo -n "  kConnectionSupportsMonitorEdidAccess remaining: "
grep -c "kConnectionSupportsMonitorEdidAccess" FakeIrisXEFramebuffer.cpp || echo "0"

echo ""
echo "══════════════════════════════════════════════════"
echo " Now run: ./build.sh 2>&1 | tee /tmp/build3.txt"
echo "══════════════════════════════════════════════════"
