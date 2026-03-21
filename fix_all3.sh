#!/bin/bash
# fix_all3.sh — fixes final compile error in FakeIrisXEFramebuffer
# Run from project root: cd /Users/jkbuha/Sources/FakeIrisXEFramebuffer && bash fix_all3.sh
set -euo pipefail

echo "Fixing FakeIrisXEFramebuffer — round 3..."
cp FakeIrisXEFramebuffer.cpp FakeIrisXEFramebuffer.cpp.bak3
cp FakeIrisXEFramebuffer.hpp FakeIrisXEFramebuffer.hpp.bak3

python3 - << 'PYEOF'
# ── Fix 1: Add getPixelFormatsForDisplayMode declaration to .hpp ──────────
with open("FakeIrisXEFramebuffer.hpp", "r") as f:
    hpp = f.read()

target = "    virtual IOReturn    connectFlags(IOIndex connectIndex,"
replacement = (
    "    /* getPixelFormatsForDisplayMode — pure virtual in Tahoe SDK IOFramebuffer.\n"
    "     * Returns a bitmask of supported pixel format indices for the given mode/depth. */\n"
    "    virtual UInt64      getPixelFormatsForDisplayMode(IODisplayModeID displayMode,\n"
    "                                                       IOIndex depth) override;\n\n"
    "    virtual IOReturn    connectFlags(IOIndex connectIndex,"
)

if target in hpp:
    hpp = hpp.replace(target, replacement, 1)
    with open("FakeIrisXEFramebuffer.hpp", "w") as f:
        f.write(hpp)
    print("FIX 1: getPixelFormatsForDisplayMode declaration added to .hpp")
else:
    print("FIX 1: connectFlags target not found in hpp — may already be present")

# ── Fix 2: Add getPixelFormatsForDisplayMode implementation to .cpp ───────
with open("FakeIrisXEFramebuffer.cpp", "r") as f:
    cpp = f.read()

# Insert implementation before connectFlags
target2 = "IOReturn FakeIrisXEFramebuffer::connectFlags("
impl = (
    "UInt64 FakeIrisXEFramebuffer::getPixelFormatsForDisplayMode(IODisplayModeID modeID,\n"
    "                                                              IOIndex depth) {\n"
    "    /* Return bit 0 set = pixel format index 0 (IO32BitDirectPixels) is supported */\n"
    "    return 0x1;\n"
    "}\n\n"
)

if target2 in cpp:
    if "getPixelFormatsForDisplayMode" not in cpp:
        cpp = cpp.replace(target2, impl + target2, 1)
        print("FIX 2: getPixelFormatsForDisplayMode implementation added to .cpp")
    else:
        print("FIX 2: implementation already present in .cpp")
else:
    print("FIX 2: connectFlags target not found in cpp")

# ── Fix 3: bytesPerPlane size_t -> UInt32 truncation warning ─────────────
cpp = cpp.replace(
    "    pixelInfo->bytesPerPlane     = _fbSize;",
    "    pixelInfo->bytesPerPlane     = (UInt32)_fbSize;",
    1
)
print("FIX 3: bytesPerPlane cast applied")

with open("FakeIrisXEFramebuffer.cpp", "w") as f:
    f.write(cpp)

PYEOF

echo ""
echo "Verification:"
echo -n "  getPixelFormatsForDisplayMode in hpp: "
grep -c "getPixelFormatsForDisplayMode" FakeIrisXEFramebuffer.hpp
echo -n "  getPixelFormatsForDisplayMode in cpp: "
grep -c "getPixelFormatsForDisplayMode" FakeIrisXEFramebuffer.cpp

echo ""
echo "Run: ./build.sh 2>&1 | tee /tmp/build4.txt"
