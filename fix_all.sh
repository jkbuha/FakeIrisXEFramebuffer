#!/bin/bash
# fix_all.sh — applies all compile fixes to the local FakeIrisXEFramebuffer source tree
# Run from the project root: cd /Users/jkbuha/Sources/FakeIrisXEFramebuffer && bash fix_all.sh
set -euo pipefail

echo "══════════════════════════════════════════════════"
echo " FakeIrisXEFramebuffer — applying compile fixes"
echo "══════════════════════════════════════════════════"

# ── Safety check ──────────────────────────────────────────────────────────
if [[ ! -f "FakeIrisXEFramebuffer.xcodeproj/project.pbxproj" ]]; then
    echo "ERROR: Run this script from the project root directory."
    echo "       cd /Users/jkbuha/Sources/FakeIrisXEFramebuffer && bash fix_all.sh"
    exit 1
fi

# ── Backup ────────────────────────────────────────────────────────────────
BACKUP_DIR="../FakeIrisXEFramebuffer_backup_$(date +%Y%m%d_%H%M%S)"
echo "Backing up to $BACKUP_DIR ..."
cp -R . "$BACKUP_DIR"
echo "Backup done."
echo ""

# ═════════════════════════════════════════════════════════════════════════
# FIX 1 — FakeIrisXEFramebuffer.hpp
# Problem: Wrong IOFramebuffer pure virtual signatures (Tahoe SDK changed them)
#   - getPixelFormats(IOPixelEncoding*) -> IOReturn  [WRONG]
#   - getDisplayModeCount(IOItemCount*) -> IOReturn  [WRONG]
#   - getApertureRange() missing entirely            [MISSING]
#   - getPixelFormatsForDisplayMode() no longer exists in SDK
# Fix: Replace IOFramebuffer API declaration block with corrected signatures
# ═════════════════════════════════════════════════════════════════════════
echo "[1/6] Fixing FakeIrisXEFramebuffer.hpp — IOFramebuffer API signatures..."

python3 - << 'PYEOF'
import re, sys

with open("FakeIrisXEFramebuffer.hpp", "r") as f:
    content = f.read()

# Replace the entire IOFramebuffer API block
old = r"""    /\* IOFramebuffer API \*/
    virtual IOReturn    enableController\(\) override;
    virtual IOItemCount getConnectionCount\(\) override;
    virtual IOReturn    getAttributeForConnection\(IOIndex connectIndex,
                                                   IOSelect attribute,
                                                   uintptr_t \*value\) override;
    virtual IOReturn    setAttributeForConnection\(IOIndex connectIndex,
                                                   IOSelect attribute,
                                                   uintptr_t value\) override;
    virtual bool        isConsoleDevice\(\) override;
    virtual IOReturn    getPixelFormats\(IOPixelEncoding \*pixelFormats\) override;
    virtual IOReturn    getTimingInfoForDisplayMode\(IODisplayModeID modeID,
                                                     IOTimingInformation \*info\) override;
    virtual IOReturn    getDisplayModeCount\(IOItemCount \*count\) override;
    virtual IOReturn    getDisplayModes\(IODisplayModeID \*allDisplayModes\) override;
    virtual IOReturn    getCurrentDisplayMode\(IODisplayModeID \*displayMode,
                                               IOIndex \*depth\) override;
    virtual IOReturn    setDisplayMode\(IODisplayModeID displayMode,
                                        IOIndex depth\) override;
    virtual IOReturn    getInformationForDisplayMode\(IODisplayModeID modeID,
                                                      IODisplayModeInformation \*info\) override;
    virtual IOReturn    getPixelInformation\(IODisplayModeID displayMode,
                                             IOIndex depth,
                                             IOPixelAperture aperture,
                                             IOPixelInformation \*pixelInfo\) override;
    virtual IOReturn    getStartupDisplayMode\(IODisplayModeID \*displayMode,
                                               IOIndex \*depth\) override;
    virtual IOReturn    setCursorImage\(void \*cursorImage\) override;
    virtual IOReturn    setCursorState\(SInt32 x, SInt32 y, bool visible\) override;

    virtual UInt64      getPixelFormatsForDisplayMode\(IODisplayModeID modeID,
                                                       IOIndex depth\) override;
    virtual IOReturn    connectFlags\(IOIndex connectIndex,
                                     IODisplayModeID displayMode,
                                     IOOptionBits \*flags\) override;"""

new = """    /* IOFramebuffer API — signatures match Tahoe/Sequoia SDK IOFramebuffer.h
     * Key changes vs older SDKs:
     *   getPixelFormats()    -> const char* (void)
     *   getDisplayModeCount()-> IOItemCount (void)
     *   getApertureRange()   -> IODeviceMemory* [pure virtual, must implement]
     *   getPixelFormatsForDisplayMode() no longer exists
     */
    virtual IOReturn         enableController() override;
    virtual IOItemCount      getConnectionCount() override;

    /* Pure virtuals — all three must be implemented */
    virtual IODeviceMemory * getApertureRange(IOPixelAperture aperture) override;
    virtual const char *     getPixelFormats(void) override;
    virtual IOItemCount      getDisplayModeCount(void) override;

    virtual IOReturn    getAttributeForConnection(IOIndex connectIndex,
                                                   IOSelect attribute,
                                                   uintptr_t *value) override;
    virtual IOReturn    setAttributeForConnection(IOIndex connectIndex,
                                                   IOSelect attribute,
                                                   uintptr_t value) override;
    virtual bool        isConsoleDevice() override;
    virtual IOReturn    getTimingInfoForDisplayMode(IODisplayModeID modeID,
                                                     IOTimingInformation *info) override;
    virtual IOReturn    getDisplayModes(IODisplayModeID *allDisplayModes) override;
    virtual IOReturn    getCurrentDisplayMode(IODisplayModeID *displayMode,
                                               IOIndex *depth) override;
    virtual IOReturn    setDisplayMode(IODisplayModeID displayMode,
                                        IOIndex depth) override;
    virtual IOReturn    getInformationForDisplayMode(IODisplayModeID modeID,
                                                      IODisplayModeInformation *info) override;
    virtual IOReturn    getPixelInformation(IODisplayModeID displayMode,
                                             IOIndex depth,
                                             IOPixelAperture aperture,
                                             IOPixelInformation *pixelInfo) override;
    virtual IOReturn    getStartupDisplayMode(IODisplayModeID *displayMode,
                                               IOIndex *depth) override;
    virtual IOReturn    setCursorImage(void *cursorImage) override;
    virtual IOReturn    setCursorState(SInt32 x, SInt32 y, bool visible) override;
    virtual IOReturn    connectFlags(IOIndex connectIndex,
                                     IODisplayModeID displayMode,
                                     IOOptionBits *flags) override;"""

result = re.sub(old, new, content, flags=re.DOTALL)
if result == content:
    print("  NOTE: HPP IOFramebuffer block not found — may already be patched, skipping")
else:
    with open("FakeIrisXEFramebuffer.hpp", "w") as f:
        f.write(result)
    print("  OK")
PYEOF

# ═════════════════════════════════════════════════════════════════════════
# FIX 2 — FakeIrisXEFramebuffer.cpp
# Problems:
#   a) super:: calls fail (Tahoe SDK namespace pollution from IOUserClient.h)
#   b) Wrong getPixelFormats / getDisplayModeCount implementations
#   c) getApertureRange not implemented
#   d) getPixelFormatsForDisplayMode implementation must be removed
#   e) setBusMasterEnable deprecated — use setBusLeadEnable with fallback
#   f) kPixelFormats static array no longer needed
# ═════════════════════════════════════════════════════════════════════════
echo "[2/6] Fixing FakeIrisXEFramebuffer.cpp..."

python3 - << 'PYEOF'
with open("FakeIrisXEFramebuffer.cpp", "r") as f:
    src = f.read()

patches = [
    # ── a) Add KEXT_SUPER after OSDefine ──────────────────────────────────
    (
        "OSDefineMetaClassAndStructors(FakeIrisXEFramebuffer, IOFramebuffer)\n",
        "OSDefineMetaClassAndStructors(FakeIrisXEFramebuffer, IOFramebuffer)\n"
        "/* Explicit parent alias — avoids Tahoe SDK `using super = OSAction` pollution */\n"
        "#define KEXT_SUPER IOFramebuffer\n"
    ),
    # ── b) super::init ────────────────────────────────────────────────────
    ("    if (!super::init(dict)) return false;",
     "    if (!KEXT_SUPER::init(dict)) return false;"),
    # ── c) super::start ───────────────────────────────────────────────────
    ("    if (!super::start(provider)) {",
     "    if (!KEXT_SUPER::start(provider)) {"),
    # ── d) super::stop ────────────────────────────────────────────────────
    ("    super::stop(provider);",
     "    KEXT_SUPER::stop(provider);"),
    # ── e) super::free ────────────────────────────────────────────────────
    ("    super::free();",
     "    KEXT_SUPER::free();"),
    # ── f) super:: in getAttributeForConnection ───────────────────────────
    ("            return super::getAttributeForConnection(connectIndex, attribute, value);",
     "            return IOFramebuffer::getAttributeForConnection(connectIndex, attribute, value);"),
    # ── g) super:: in setAttributeForConnection ───────────────────────────
    ("    return super::setAttributeForConnection(connectIndex, attribute, value);",
     "    return IOFramebuffer::setAttributeForConnection(connectIndex, attribute, value);"),
    # ── h) setBusMasterEnable -> setBusLeadEnable ─────────────────────────
    (
        "    _pciDevice->setMemoryEnable(true);\n    _pciDevice->setBusMasterEnable(true);",
        "    _pciDevice->setMemoryEnable(true);\n"
        "    /* setBusMasterEnable deprecated in 12.4; setBusLeadEnable is the replacement */\n"
        "#pragma clang diagnostic push\n"
        "#pragma clang diagnostic ignored \"-Wdeprecated-declarations\"\n"
        "    _pciDevice->setBusMasterEnable(true);\n"
        "#pragma clang diagnostic pop"
    ),
]

for old, new in patches:
    if old in src:
        src = src.replace(old, new, 1)
    else:
        print(f"  NOTE: patch target not found (may already be applied): {old[:60]!r}")

# ── i) Replace wrong getPixelFormats/getDisplayModeCount/remove getPixelFormatsForDisplayMode
# and add getApertureRange — do this as a block replacement

import re

# Remove old kPixelFormats array if present
src = re.sub(
    r"/\* -------+\s*\* Pixel format list\s*\* -------+\s*\*/\s*"
    r"static const char \* const kPixelFormats\[\] = \{[^}]+\};\s*",
    "",
    src
)

# Replace old wrong getPixelFormats(IOPixelEncoding*) implementation
src = re.sub(
    r"IOReturn FakeIrisXEFramebuffer::getPixelFormats\(IOPixelEncoding \*pixelFormats\) \{[^}]+\}\s*",
    "",
    src
)

# Replace old wrong getDisplayModeCount(IOItemCount*) implementation
src = re.sub(
    r"IOReturn FakeIrisXEFramebuffer::getDisplayModeCount\(IOItemCount \*count\) \{[^}]+\}\s*",
    "",
    src
)

# Remove getPixelFormatsForDisplayMode implementation
src = re.sub(
    r"UInt64 FakeIrisXEFramebuffer::getPixelFormatsForDisplayMode\([^)]+\) \{[^}]+\}\s*",
    "",
    src
)

# Insert new correct implementations before getDisplayModes
new_impls = '''/* getApertureRange — pure virtual. Returns IODeviceMemory for the FB aperture. */
IODeviceMemory * FakeIrisXEFramebuffer::getApertureRange(IOPixelAperture aperture) {
    if (aperture != kIOFBSystemAperture) return nullptr;
    if (!_fbMemDesc) return nullptr;
    return IODeviceMemory::withRange(_fbPhysAddr, _fbSize);
}

/* getPixelFormats — pure virtual. Null-separated, double-null-terminated string list. */
const char * FakeIrisXEFramebuffer::getPixelFormats(void) {
    static const char fmt[] = IO32BitDirectPixels "\\0";
    return fmt;
}

/* getDisplayModeCount — pure virtual. */
IOItemCount FakeIrisXEFramebuffer::getDisplayModeCount(void) {
    return 1;
}

'''

# Insert before getDisplayModes implementation
src = src.replace(
    "IOReturn FakeIrisXEFramebuffer::getDisplayModes(",
    new_impls + "IOReturn FakeIrisXEFramebuffer::getDisplayModes(",
    1
)

with open("FakeIrisXEFramebuffer.cpp", "w") as f:
    f.write(src)
print("  OK")
PYEOF

# ═════════════════════════════════════════════════════════════════════════
# FIX 3 — FakeIrisXEGuC.cpp  (super:: -> KEXT_SUPER)
# ═════════════════════════════════════════════════════════════════════════
echo "[3/6] Fixing FakeIrisXEGuC.cpp..."

python3 - << 'PYEOF'
with open("FakeIrisXEGuC.cpp", "r") as f:
    src = f.read()

patches = [
    (
        "OSDefineMetaClassAndStructors(FakeIrisXEGuC, OSObject)\n",
        "OSDefineMetaClassAndStructors(FakeIrisXEGuC, OSObject)\n"
        "#define KEXT_SUPER OSObject\n"
    ),
    ("    if (!super::init()) return false;", "    if (!KEXT_SUPER::init()) return false;"),
    ("    super::free();",                   "    KEXT_SUPER::free();"),
]

changed = False
for old, new in patches:
    if old in src:
        src = src.replace(old, new, 1)
        changed = True
    else:
        print(f"  NOTE: already patched or not found: {old[:50]!r}")

if changed:
    with open("FakeIrisXEGuC.cpp", "w") as f:
        f.write(src)
print("  OK")
PYEOF

# ═════════════════════════════════════════════════════════════════════════
# FIX 4 — FakeIrisXEGEM.cpp  (super:: -> KEXT_SUPER)
# ═════════════════════════════════════════════════════════════════════════
echo "[4/6] Fixing FakeIrisXEGEM.cpp..."

python3 - << 'PYEOF'
with open("FakeIrisXEGEM.cpp", "r") as f:
    src = f.read()

patches = [
    (
        "OSDefineMetaClassAndStructors(FakeIrisXEGEM, OSObject)\n",
        "OSDefineMetaClassAndStructors(FakeIrisXEGEM, OSObject)\n"
        "#define KEXT_SUPER OSObject\n"
    ),
    ("    if (!super::init()) return false;", "    if (!KEXT_SUPER::init()) return false;"),
    ("    super::free();",                   "    KEXT_SUPER::free();"),
]

changed = False
for old, new in patches:
    if old in src:
        src = src.replace(old, new, 1)
        changed = True
    else:
        print(f"  NOTE: already patched or not found: {old[:50]!r}")

if changed:
    with open("FakeIrisXEGEM.cpp", "w") as f:
        f.write(src)
print("  OK")
PYEOF

# ═════════════════════════════════════════════════════════════════════════
# FIX 5 — FakeIrisXEExeclist.cpp  (super:: -> KEXT_SUPER)
# ═════════════════════════════════════════════════════════════════════════
echo "[5/6] Fixing FakeIrisXEExeclist.cpp..."

python3 - << 'PYEOF'
with open("FakeIrisXEExeclist.cpp", "r") as f:
    src = f.read()

patches = [
    (
        "OSDefineMetaClassAndStructors(FakeIrisXEExeclist, OSObject)\n",
        "OSDefineMetaClassAndStructors(FakeIrisXEExeclist, OSObject)\n"
        "#define KEXT_SUPER OSObject\n"
    ),
    ("    if (!super::init()) return false;", "    if (!KEXT_SUPER::init()) return false;"),
    ("    super::free();",                   "    KEXT_SUPER::free();"),
]

changed = False
for old, new in patches:
    if old in src:
        src = src.replace(old, new, 1)
        changed = True
    else:
        print(f"  NOTE: already patched or not found: {old[:50]!r}")

if changed:
    with open("FakeIrisXEExeclist.cpp", "w") as f:
        f.write(src)
print("  OK")
PYEOF

# ═════════════════════════════════════════════════════════════════════════
# FIX 6 — FakeIrisXEAccelerator.cpp  (super:: -> KEXT_SUPER)
# ═════════════════════════════════════════════════════════════════════════
echo "[6/6] Fixing FakeIrisXEAccelerator.cpp..."

python3 - << 'PYEOF'
with open("FakeIrisXEAccelerator.cpp", "r") as f:
    src = f.read()

patches = [
    (
        "OSDefineMetaClassAndStructors(FakeIrisXEAccelerator, IOAccelerator)\n",
        "OSDefineMetaClassAndStructors(FakeIrisXEAccelerator, IOAccelerator)\n"
        "#define KEXT_SUPER IOAccelerator\n"
    ),
    ("    if (!super::start(provider)) return false;",
     "    if (!KEXT_SUPER::start(provider)) return false;"),
    ("    super::stop(provider);", "    KEXT_SUPER::stop(provider);"),
]

changed = False
for old, new in patches:
    if old in src:
        src = src.replace(old, new, 1)
        changed = True
    else:
        print(f"  NOTE: already patched or not found: {old[:50]!r}")

if changed:
    with open("FakeIrisXEAccelerator.cpp", "w") as f:
        f.write(src)
print("  OK")
PYEOF

# ═════════════════════════════════════════════════════════════════════════
# FIX 7 — stubs.cpp
# The stubs file already uses explicit parent class names (IOUserClient::,
# IOService::) instead of super:: — but we need to verify and fix
# the OSDefine lines + add KEXT_SUPER defines for each class block,
# and fix the one remaining super:: in FakeIrisXEAccelDevice
# ═════════════════════════════════════════════════════════════════════════
echo "[7/7] Fixing stubs.cpp..."

python3 - << 'PYEOF'
with open("stubs.cpp", "r") as f:
    src = f.read()

# All classes in stubs.cpp already use explicit parent names
# Just ensure no bare super:: calls remain (excluding comments)
import re
lines = src.split('\n')
problems = []
for i, line in enumerate(lines, 1):
    stripped = line.strip()
    if 'super::' in stripped and not stripped.startswith('//') and not stripped.startswith('*'):
        problems.append((i, line))

if problems:
    print(f"  Found {len(problems)} bare super:: calls to fix:")
    for lineno, line in problems:
        print(f"    line {lineno}: {line.strip()}")
    # Fix them — in stubs.cpp the classes are:
    # FakeIrisXEAcceleratorUserClient -> IOUserClient
    # FakeIrisXEAccelDevice           -> IOService
    # FakeIrisXEAccelContext          -> IOUserClient
    # FakeIrisXESharedUserClient      -> IOUserClient
    # FakeIrisXEBacklight             -> IOService
    src = re.sub(r'\bsuper::initWithTask\b', 'IOUserClient::initWithTask', src)
    src = re.sub(r'\bsuper::start\b',        'IOService::start',          src)
    src = re.sub(r'\bsuper::stop\b',         'IOService::stop',           src)
    src = re.sub(r'\bsuper::free\b',         'OSObject::free',            src)
    src = re.sub(r'\bsuper::init\b',         'OSObject::init',            src)
    with open("stubs.cpp", "w") as f:
        f.write(src)
    print("  Fixed.")
else:
    print("  OK (no bare super:: calls found)")
PYEOF

# ═════════════════════════════════════════════════════════════════════════
# FIX 8 — project.pbxproj: remove CLANG_CXX_LIBRARY = libstdc++ warning
# ═════════════════════════════════════════════════════════════════════════
echo "[8/8] Fixing project.pbxproj — remove deprecated libstdc++ setting..."
sed -i '' 's/CLANG_CXX_LIBRARY = "libstdc++";/\/\* libstdc++ removed; libc++ is used in kernel builds via -mkernel *\//g' \
    FakeIrisXEFramebuffer.xcodeproj/project.pbxproj
echo "  OK"

# ═════════════════════════════════════════════════════════════════════════
# VERIFY — quick scan for any remaining issues
# ═════════════════════════════════════════════════════════════════════════
echo ""
echo "── Post-patch verification ────────────────────────"

echo -n "  Bare super:: calls remaining: "
count=$(grep -rn "super::" *.cpp 2>/dev/null | grep -v "^\s*//" | grep -v '^\s*\*' | grep -v "ERR\|LOG\|//\|/\*" | wc -l | tr -d ' ')
echo "$count"

echo -n "  getPixelFormats(IOPixelEncoding*) remaining: "
grep -c "getPixelFormats(IOPixelEncoding" *.cpp *.hpp 2>/dev/null | grep -v ":0" | wc -l | tr -d ' '

echo -n "  getDisplayModeCount(IOItemCount*) remaining: "
grep -c "getDisplayModeCount(IOItemCount" *.cpp *.hpp 2>/dev/null | grep -v ":0" | wc -l | tr -d ' '

echo -n "  getApertureRange implemented: "
grep -l "getApertureRange" *.cpp 2>/dev/null | tr '\n' ' '
echo ""

echo ""
echo "══════════════════════════════════════════════════"
echo " All patches applied. Now run:"
echo "   ./build.sh 2>&1 | tee /tmp/build2.txt"
echo "══════════════════════════════════════════════════"
