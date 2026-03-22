#!/bin/bash
# generate_stubs.sh — creates iogfx_stubs.o needed to link FakeIrisXEFramebuffer
# against IOGraphicsFamily symbols that are in the kernelcache but not on disk.
#
# Must be run AFTER a successful build (needs the .kext binary to extract symbols from).
# The generated iogfx_stubs.o is committed to the repo so it survives reboots.
#
# Run from project root: cd /Users/jkbuha/Sources/FakeIrisXEFramebuffer && bash generate_stubs.sh

set -euo pipefail

BINARY="build/Debug/FakeIrisXEFramebuffer.kext/Contents/MacOS/FakeIrisXEFramebuffer"
STUB_S="iogfx_stubs.s"
STUB_O="iogfx_stubs.o"

if [[ ! -f "$BINARY" ]]; then
    echo "ERROR: Binary not found at $BINARY"
    echo "       Run a build first (even a failing link is OK — we just need .o files)"
    echo "       Or use the hardcoded symbol list below."
fi

echo "Extracting undefined symbols from binary..."

# Use hardcoded list of known-needed symbols as baseline
# (covers the case where binary doesn't exist yet)
cat > /tmp/known_syms.txt << 'SYMS'
__ZN13IOFramebuffer10gMetaClassE
__ZN13IOAccelerator10gMetaClassE
__ZN13IOFramebuffer10superClassE
__ZN13IOAccelerator10superClassE
__ZTV13IOFramebuffer
__ZTV13IOAccelerator
__ZN13IOFramebufferC2EPK11OSMetaClass
__ZN13IOFramebufferD2Ev
__ZN13IOFramebuffer4openEv
__ZN13IOFramebuffer5closeEv
__ZN13IOFramebuffer6attachEP9IOService
__ZN13IOFramebuffer7messageEjP9IOServicePv
__ZN13IOFramebuffer9terminateEj
__ZN13IOFramebuffer9setNumberEP12OSDictionaryPKcj
__ZN13IOFramebuffer10hideCursorEv
__ZN13IOFramebuffer10moveCursorEP8IOGPointi
__ZN13IOFramebuffer10showCursorEP8IOGPointi
__ZN13IOFramebuffer10setDDCDataEij
__ZN13IOFramebuffer10getVBLTimeEPyS0_
__ZN13IOFramebuffer11flushCursorEv
__ZN13IOFramebuffer11getDDCBlockEijjjPhPy
__ZN13IOFramebuffer11readDDCDataEi
__ZN13IOFramebuffer11setDDCClockEij
__ZN13IOFramebuffer12didTerminateEP9IOServicejPb
__ZN13IOFramebuffer12doI2CRequestEjP14IOI2CBusTimingP12IOI2CRequest
__ZN13IOFramebuffer12getAttributeEjPm
__ZN13IOFramebuffer12getVRAMRangeEv
__ZN13IOFramebuffer12readDDCClockEi
__ZN13IOFramebuffer12requestProbeEj
__ZN13IOFramebuffer12setAttributeEjm
__ZN13IOFramebuffer13getAppleSenseEiPjS0_S0_S0_
__ZN13IOFramebuffer13hasDDCConnectEi
__ZN13IOFramebuffer13newUserClientEP4taskPvjPP12IOUserClient
__ZN13IOFramebuffer13serializeInfoEP11OSSerialize
__ZN13IOFramebuffer13setGammaTableEjjjPv
__ZN13IOFramebuffer13setGammaTableEjjjPvb
__ZN13IOFramebuffer13setPowerStateEmP9IOService
__ZN13IOFramebuffer13setPropertiesEP8OSObject
__ZN13IOFramebuffer13willTerminateEP9IOServicej
__ZN13IOFramebuffer14diagnoseReportEPvS0_S0_S0_
__ZN13IOFramebuffer15enableDDCRasterEb
__ZN13IOFramebuffer15getBoundingRectEPP9IOGBounds
__ZN13IOFramebuffer16requestTerminateEP9IOServicej
__ZN13IOFramebuffer17getAggressivenessEmPm
__ZN13IOFramebuffer17setAggressivenessEmm
__ZN13IOFramebuffer17setApertureEnableEij
__ZN13IOFramebuffer17setInterruptStateEPvj
__ZN13IOFramebuffer18convertCursorImageEPvP26IOHardwareCursorDescriptorP20IOHardwareCursorInfo
__ZN13IOFramebuffer18setCLUTWithEntriesEP12IOColorEntryjjj
__ZN13IOFramebuffer18setDetailedTimingsEP7OSArray
__ZN13IOFramebuffer19unregisterInterruptEPv
__ZN13IOFramebuffer20callPlatformFunctionEPK8OSSymbolbPvS3_S3_S3_
__ZN13IOFramebuffer21powerStateDidChangeToEmmP9IOService
__ZN13IOFramebuffer21setStartupDisplayModeEii
__ZN13IOFramebuffer21setupForCurrentConfigEv
__ZN13IOFramebuffer22powerStateWillChangeToEmmP9IOService
__ZN13IOFramebuffer22validateDetailedTimingEPvy
__ZN13IOFramebuffer23_RESERVEDIOFramebuffer3Ev
__ZN13IOFramebuffer23_RESERVEDIOFramebuffer4Ev
__ZN13IOFramebuffer23_RESERVEDIOFramebuffer5Ev
__ZN13IOFramebuffer23_RESERVEDIOFramebuffer6Ev
__ZN13IOFramebuffer23_RESERVEDIOFramebuffer7Ev
__ZN13IOFramebuffer23_RESERVEDIOFramebuffer8Ev
__ZN13IOFramebuffer23_RESERVEDIOFramebuffer9Ev
__ZN13IOFramebuffer24_RESERVEDIOFramebuffer10Ev
__ZN13IOFramebuffer24_RESERVEDIOFramebuffer11Ev
__ZN13IOFramebuffer24_RESERVEDIOFramebuffer12Ev
__ZN13IOFramebuffer24_RESERVEDIOFramebuffer13Ev
__ZN13IOFramebuffer24_RESERVEDIOFramebuffer14Ev
__ZN13IOFramebuffer24_RESERVEDIOFramebuffer15Ev
__ZN13IOFramebuffer24_RESERVEDIOFramebuffer16Ev
__ZN13IOFramebuffer24_RESERVEDIOFramebuffer17Ev
__ZN13IOFramebuffer24_RESERVEDIOFramebuffer18Ev
__ZN13IOFramebuffer24_RESERVEDIOFramebuffer19Ev
__ZN13IOFramebuffer24_RESERVEDIOFramebuffer20Ev
__ZN13IOFramebuffer24_RESERVEDIOFramebuffer21Ev
__ZN13IOFramebuffer24_RESERVEDIOFramebuffer22Ev
__ZN13IOFramebuffer24_RESERVEDIOFramebuffer23Ev
__ZN13IOFramebuffer24_RESERVEDIOFramebuffer24Ev
__ZN13IOFramebuffer24_RESERVEDIOFramebuffer25Ev
__ZN13IOFramebuffer24_RESERVEDIOFramebuffer26Ev
__ZN13IOFramebuffer24_RESERVEDIOFramebuffer27Ev
__ZN13IOFramebuffer24_RESERVEDIOFramebuffer28Ev
__ZN13IOFramebuffer24_RESERVEDIOFramebuffer29Ev
__ZN13IOFramebuffer24_RESERVEDIOFramebuffer30Ev
__ZN13IOFramebuffer24_RESERVEDIOFramebuffer31Ev
__ZN13IOFramebuffer24getNotificationSemaphoreEjPP9semaphore
__ZN13IOFramebuffer24registerForInterruptTypeEjPFvP8OSObjectPvES1_S2_PS2_
__ZNK13IOFramebuffer11getWorkLoopEv
SYMS

# If binary exists, merge with any additional undefined symbols from it
if [[ -f "$BINARY" ]]; then
    nm "$BINARY" | grep " U " | awk '{print $2}' >> /tmp/known_syms.txt
fi

# Deduplicate
sort -u /tmp/known_syms.txt > /tmp/all_syms.txt
SYM_COUNT=$(wc -l < /tmp/all_syms.txt | tr -d ' ')
echo "Generating stub with $SYM_COUNT symbols..."

python3 - << 'EOF'
with open("/tmp/all_syms.txt") as f:
    syms = [s.strip() for s in f if s.strip()]

asm = "/* iogfx_stubs.s — auto-generated by generate_stubs.sh */\n"
asm += "/* Provides stub symbols for IOGraphicsFamily/IOAcceleratorFamily2 */\n"
asm += "/* which are in the kernelcache but not linkable on-disk in Sequoia+ */\n\n"
asm += ".data\n"
for sym in syms:
    asm += f".globl {sym}\n{sym}:\n  .quad 0\n"

with open("iogfx_stubs.s", "w") as f:
    f.write(asm)
print(f"  Written iogfx_stubs.s ({len(syms)} symbols)")
EOF

# Assemble
as -arch x86_64 "$STUB_S" -o "$STUB_O"

echo ""
echo "Generated:"
echo "  $STUB_S  ($(wc -l < $STUB_S | tr -d ' ') lines)"
echo "  $STUB_O  ($(nm $STUB_O | wc -l | tr -d ' ') symbols)"
echo ""
echo "Build with:"
echo "  OTHER_LDFLAGS=\"\$(pwd)/iogfx_stubs.o\""
