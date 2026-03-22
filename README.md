# FakeIrisXEFramebuffer.kext

> DISCLAIMER: This is mostly AI-driven. I'm just giving AI a bone and seeing just
> with [Claude](https://claude.ai) how far we can actually get to automating resolving
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
| **Kext loads in macOS Sequoia 15** | ✅ Working | Survived reboot, stable |
| **IOKit device match** | ✅ Working | Matches `pci8086,9a49` (i7-1165G7) |
| **IOService registered + active** | ✅ Working | `registered, matched, active` in ioreg |
| **AGPM integration** | ✅ Working | Apple GPU Power Management attached as child |
| **AppleMCCS child** | ✅ Working | Monitor control module attached |
| **MMIO / BAR0 mapping** | ✅ In `start()` | Safe — no register access at boot |
| **Hardware init (GT/power wells/display)** | 🔶 Deferred | Moved to `enableController()` |
| **`enableController()` called by WindowServer** | ⏳ Not yet | NDRV framebuffer wins display race |
| **FORCEWAKE / GT response** | ⏳ Untested | Awaits `enableController()` |
| **Display pipeline** | ⏳ Untested | Pipe A / Trans A / Plane 1A code written |
| **GuC firmware** | 🔶 Stub | Needs `tgl_guc_70.bin` embedded |
| **Metal / hardware acceleration** | ❌ Not started | Requires private framework reverse-eng |

---

## How It Works

The kext is a pure `IOFramebuffer` subclass. It matches the Tiger Lake PCI device
(`0x9A498086` and related IDs) and registers with IOKit. Hardware initialisation
is **fully deferred** to `enableController()` to avoid boot hangs — no hardware
access occurs in `start()`.

Currently the system boots with `IONDRVFramebuffer` (the EFI/firmware framebuffer)
driving the display, and our kext sits alongside it as a registered but inactive
`IOFramebuffer`. The next step is to raise our `IOProbeScore` to beat NDRV and
get WindowServer to call `enableController()`.

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
igfxframe=0xFFFFFFFF agdpmod=pikera
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

- [ ] **IOKit test tool** — user client to verify FORCEWAKE + GT response without display risk
- [ ] **Beat NDRV** — raise `IOProbeScore`, implement `getApertureRange()`, trigger `enableController()`
- [ ] **Hardware validation** — FORCEWAKE, power wells, Pipe A, display output
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
