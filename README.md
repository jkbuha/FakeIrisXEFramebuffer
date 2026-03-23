# FakeIrisXEFramebuffer.kext

> 🤖 **Live development log:** This project is being actively developed in collaboration
> with [Claude](https://claude.ai) (Anthropic) to see how far we can get resolving
> Tiger Lake iGPU support on macOS entirely through AI-assisted kernel development.
> The conversation history and decision trail are embedded in the commit log.

**Experimental macOS kernel extension for Intel Tiger Lake Iris Xe iGPU (11th Generation)**  
Target: macOS Sequoia 15 · x86_64 hackintosh · OpenCore

> ⚠️ **Experimental research driver.** This kext loads and matches the Tiger Lake GPU
> in IOKit but does not yet provide hardware acceleration or drive the display.
> Expect kernel panics if you push beyond the current stable state.

---

## Current Status

| Milestone | Status | Notes |
|---|---|---|
| **Kext loads in macOS Sequoia 15** | ✅ Working | Survived reboot, stable via launchd auto-load |
| **IOKit device match** | ✅ Working | Matches spoofed `pci8086,8a52` after OC device-id injection |
| **IOService registered + active** | ✅ Working | `registered, matched, active` in ioreg |
| **AGPM integration** | ✅ Working | Apple GPU Power Management attached as child |
| **AppleMCCS child** | ✅ Working | Monitor control module attached |
| **MMIO / BAR0 mapping** | ✅ Working | BAR0 mapped in `start()`; IORegistry diagnostics published |
| **CD clock reprogramming** | ✅ Working | Reprogrammed 172.8→652.8 MHz in `start()` via PLL disable/ratio/enable sequence |
| **CD clock confirmed at 652.8 MHz** | ✅ Confirmed | `CDCLK_CTL` field reads `0x518` after reprogram; persists across probe |
| **ICL device-id spoof** | ✅ Working | `device-id=0x8A52` injected via OpenCore DeviceProperties |
| **AppleIntelICLGraphics loaded** | ✅ Working | ICL graphics kext loads alongside our kext |
| **AppleIntelICLLPGraphicsFramebuffer loaded** | ✅ Working | ICLLP framebuffer kext loads, VRAM shows 8 MB |
| **HW test tool** | ✅ Working | `requestProbe()` triggers test; results published to IORegistry |
| **FORCEWAKE** | ✅ Working | GT responds to FORCEWAKE — hardware is alive |
| **Power wells (PW1/PW2)** | ❌ Timing out | Register address may differ on TGL vs HSW/ICL — under investigation |
| **`enableController()` called by WindowServer** | ⏳ Blocked | `IOMatchCategory=IOFramebuffer` causes boot hang; root cause under investigation |
| **Display pipeline** | ⏳ Blocked | Awaits power wells |
| **GuC firmware** | 🔶 Stub | Needs `tgl_guc_70.bin` embedded |
| **Metal / hardware acceleration** | ❌ Not started | Requires private framework reverse-eng |

---

## How It Works

The kext is a pure `IOFramebuffer` subclass. It matches the Tiger Lake PCI device
via a device-id spoof (`0x9A49` → `0x8A52` ICL) injected by OpenCore DeviceProperties,
causing `AppleIntelICLGraphics` and `AppleIntelICLLPGraphicsFramebuffer` to also load.

### Current boot sequence

1. OpenCore injects `device-id=0x8A52` and `AAPL,ig-platform-id=0x00003400`
2. `AppleIntelICLGraphics` and `AppleIntelICLLPGraphicsFramebuffer` load (VRAM: 8MB)
3. Our kext matches, maps BAR0, reprograms CD clock from 172.8→652.8 MHz
4. `IONDRVFramebuffer` drives the display (EFI framebuffer)
5. Our kext sits alongside as a registered `IOFramebuffer` in `IODefaultMatchCategory`

### Key hardware findings

- **FORCEWAKE**: GT responds correctly ✅
- **CD clock**: Firmware leaves it at 172.8 MHz; our kext reprograms to 652.8 MHz via PLL sequence ✅
- **Power wells**: Timing out — TGL power well register addresses differ from HSW/ICL baseline 🔶
- **`IOMatchCategory=IOFramebuffer`**: Causes IOKit boot deadlock when competing with NDRV — root cause unknown

### HW test tool

Results from `requestProbe()` are published to IORegistry and can be read without a user client:

```bash
sudo ./fxe_test          # triggers requestProbe, reads results
ioreg -l -p IOService -n FakeIrisXEFramebuffer | grep "FXE-"
```

---

## Build Requirements

| Requirement | Notes |
|---|---|
| macOS Sequoia 15 | Build and target OS |
| Xcode with Tahoe SDK (26.x) | Install from App Store or developer.apple.com |
| KDK 15.5 build 24F74 | Required for symbol resolution at load time |
| SIP fully disabled | `csrutil disable` + `csrutil authenticated-root disable` in Recovery |
| `amfi_get_out_of_my_way=1` boot arg | Required for unsigned kext loading |

---

## Build Instructions

### 1. Install the KDK

Download and install the Kernel Debug Kit from Dortania's mirror (no Apple account needed):

```bash
curl -L -o /tmp/KDK_15.5.dmg \
  "https://github.com/dortania/KdkSupportPkg/releases/download/24F74/Kernel_Debug_Kit_15.5_build_24F74.dmg"
hdiutil attach /tmp/KDK_15.5.dmg
sudo installer -pkg "/Volumes/Kernel Debug Kit/KernelDebugKit.pkg" -target /
```

### 2. Clone and build

```bash
git clone https://github.com/jkbuha/FakeIrisXEFramebuffer.git
cd FakeIrisXEFramebuffer

# Generate the IOGraphicsFamily symbol stub (required for linking)
bash generate_stubs.sh

# Build
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
  OTHER_LDFLAGS="$(pwd)/iogfx_stubs.o -Xlinker -kext $KMOD/libkmod.a $KMOD/libkmodc++.a"
```

Build output: `build/Debug/FakeIrisXEFramebuffer.kext`

### 3. Install to /Library/Extensions

```bash
sudo rm -rf /Library/Extensions/FakeIrisXEFramebuffer.kext
sudo cp -R build/Debug/FakeIrisXEFramebuffer.kext /Library/Extensions/
sudo cp FakeIrisXEFramebuffer.kext/Contents/Info.plist \
    /Library/Extensions/FakeIrisXEFramebuffer.kext/Contents/Info.plist
sudo chown -R root:wheel /Library/Extensions/FakeIrisXEFramebuffer.kext
```

### 4. Approve and load

```bash
sudo kmutil load --bundle-path /Library/Extensions/FakeIrisXEFramebuffer.kext
```

If you see **"not approved"**: go to System Settings → Privacy & Security → Allow.  
If you see **"requires a reboot"**: reboot normally.

### 5. Verify after reboot

```bash
kextstat | grep FakeIrisXE
ioreg -l -p IOService -n FakeIrisXEFramebuffer 2>/dev/null | head -20
```

Expected:
```
com.anomy.driver.FakeIrisXEFramebuffer (1.0.0) — loaded
<class FakeIrisXEFramebuffer, registered, matched, active>
```

---

## Why the Symbol Stub?

In macOS 13+, `IOGraphicsFamily.kext` has no on-disk binary — it lives entirely in
the sealed kernelcache. This means `kmutil` cannot resolve `IOFramebuffer` metaclass
and vtable symbols at collection-build time when loading a third-party `IOFramebuffer`
subclass.

The workaround (`iogfx_stubs.s`) provides **weak symbol definitions** for the
specific entry-point symbols (`__antimain`, `__realmain`) that the static linker
requires. All `IOFramebuffer` vtable and metaclass symbols are left **undefined**
in our binary so `kmutil` resolves them correctly from the running kernelcache.

To regenerate the stub after changing source files:
```bash
bash generate_stubs.sh
```

---

## Why Not OpenCore Injection?

OpenCore's prelinker (`OCAK`) cannot resolve `IOFramebuffer` vtable symbols at
inject time — the same root cause as above, but without access to the running
kernelcache. This produces:

```
OCAK: Vtable patching failed for kext com.anomy.driver.FakeIrisXEFramebuffer
OC: Prelinked injection FakeIrisXEFramebuffer.kext - Invalid Parameter
```

The `/Library/Extensions/` install path (loaded via `kmutil`) is the working
method. OpenCore injection would require restructuring as a Lilu plugin, which
is a future roadmap item.

---

## Required Boot Args

In `config.plist → NVRAM → boot-args`:
```
amfi_get_out_of_my_way=1 -lilubetaall -wegbeta keepsyms=1 debug=0x100
agdpmod=pikera -igfxcdc
```

Required OpenCore DeviceProperties for `PciRoot(0x0)/Pci(0x2,0x0)`:
```xml
<key>device-id</key>
<data>UooAAA==</data>           <!-- 0x8A52 LE — spoof TGL → ICL so ICLLP loads -->
<key>AAPL,ig-platform-id</key>
<data>ADQAAA==</data>           <!-- 0x00003400 LE — ICL eDP laptop platform -->
<key>framebuffer-stolenmem</key>
<data>AAAwAQ==</data>
<key>framebuffer-fbmem</key>
<data>AACQAA==</data>
```

Required OpenCore settings:
- `Kernel → Quirks → SecureBootModel`: `Disabled`
- SIP fully disabled in Recovery

---

## File Map

```
FakeIrisXEFramebuffer/
├── FakeIrisXEFramebuffer.cpp/hpp  — IOFramebuffer subclass, GT init, display pipeline
│                                    Pipe A / Trans A / Plane 1A, 1920×1080@60 eDP
│                                    All HW access deferred to enableController()
├── FakeIrisXEGuC.cpp/hpp          — GuC firmware DMA loader + CTB rings
│                                    Disabled: set GUC_FIRMWARE_AVAILABLE=1 to enable
├── FakeIrisXEExeclist.cpp/hpp     — LRC + ELSP command submission
│                                    Path A: GuC-mediated; Path B: direct ELSP fallback
├── FakeIrisXEGEM.cpp/hpp          — GGTT object allocator + PTE writer
├── FakeIrisXEAccelerator.cpp/hpp  — IOAccelerator stub (excluded from current build)
├── stubs.cpp                      — AccelDevice, AccelContext, UserClient stubs
│                                    Excluded: IOAcceleratorFamily2 not in kernelcache
├── embedded_firmware.cpp/h        — GuC firmware placeholder
├── i915_reg.h                     — TGL MMIO register definitions
│                                    FORCEWAKE, power wells, display, GGTT, Execlists
├── FakeIrisXERing.h               — Ring buffer write helpers
├── iogfx_stubs.s                  — Symbol stubs for static link (see above)
├── generate_stubs.sh              — Regenerates iogfx_stubs.o
├── FakeIrisXEFramebuffer.kext/    — Info.plist (source, copied into build output)
│   └── Contents/Info.plist
└── FakeIrisXEFramebuffer.xcodeproj/
```

---

## Supported Hardware

| Device ID | GPU | Status |
|---|---|---|
| `0x9A49` | Tiger Lake GT2 (i7-1165G7, i7-1185G7) | ✅ Tested — loads + matches |
| `0x9A40` | Tiger Lake GT1 | 🔶 Untested |
| `0x9A59` | Tiger Lake GT2 | 🔶 Untested |
| `0x9A60` | Tiger Lake GT1 | 🔶 Untested |
| `0x9A68` | Tiger Lake GT1 | 🔶 Untested |
| `0x9A78` | Tiger Lake GT2 | 🔶 Untested |

Test machine: Dell XPS 9500 (i7-1165G7), macOS Sequoia 15.7.4 (24G517)

---

## Enabling GuC Firmware (Path A)

By default, command submission uses legacy direct ELSP (Path B). To enable
GuC-mediated submission:

```bash
# 1. Get firmware
git clone https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git
xxd -i linux-firmware/i915/tgl_guc_70.bin > guc_fw_raw.cpp

# 2. Replace embedded_firmware.cpp with xxd output
# 3. Rename arrays to: gTGLGuCFirmwareData / gTGLGuCFirmwareSize
```

Set in `embedded_firmware.h`:
```c
#define GUC_FIRMWARE_AVAILABLE 1
```

Then rebuild.

---

## Roadmap

- [x] **Kext loads and matches Tiger Lake GPU** — IOService registered, matched, active
- [x] **CD clock diagnostic** — confirmed 172.8 MHz at boot (root cause of display hang)
- [x] **CD clock reprogramming** — reprogrammed to 652.8 MHz in `start()` via PLL sequence
- [x] **ICL device-id spoof** — ICLLP loads via OpenCore DeviceProperties
- [x] **FORCEWAKE** — GT confirmed alive and responsive
- [x] **HW test tool** — `requestProbe()` + IORegistry; no user client needed
- [ ] **Power wells (PW1/PW2)** — diagnose TGL register address differences vs HSW/ICL
- [ ] **`enableController()` triggered** — needs non-deadlocking NDRV suppression strategy
- [ ] **Display pipeline** — Pipe A / Trans A / Plane 1A, eDP output
- [ ] **GuC firmware** — embed `tgl_guc_70.bin`, enable Path A submission
- [ ] **Lilu plugin** — proper OpenCore injection path
- [ ] **Metal** — Apple private framework reverse engineering

---

## References

- Linux i915 driver: https://github.com/torvalds/linux/tree/master/drivers/gpu/drm/i915
- Intel Tiger Lake PRM (Vol 2c/12/15/16/17): https://01.org/linuxgraphics/documentation
- Dortania KDK mirror: https://github.com/dortania/KdkSupportPkg
- pawan295 upstream: https://github.com/pawan295/Appleinteltgldriver.kext
- InsanelyMac thread: https://www.insanelymac.com/forum/topic/358305/
- OpenCore guide: https://dortania.github.io/OpenCore-Install-Guide/

---

## License

MIT — do whatever you want, no warranty implied.
