# FakeIrisXEFramebuffer.kext
**Custom macOS kernel extension for Intel Tiger Lake Iris Xe iGPU (Gen 12)**  
Target: macOS Sequoia 15 · OpenCore · x86_64

---

## Status

| Component | Status | Notes |
|---|---|---|
| Kext load / IOService | ✅ Compilable | `start()` skeleton complete |
| MMIO / BAR0 mapping | ✅ Complete | `mapMMIO()` |
| FORCEWAKE | ✅ Complete | Render + GT domains |
| Power wells | ✅ Complete | PW1, PW2, DDI-A, AUX-A |
| Display pipeline | ✅ Complete | Pipe A, Trans A, Plane 1A, 1080p60 |
| Framebuffer alloc | ✅ Complete | Physically contiguous, XRGB8888 |
| GEM / GGTT | ✅ Compilable | Bump allocator, PTE writer |
| Execlists (direct ELSP) | ✅ Compilable | LRC init, ring emit, ELSP submit |
| Execlists (GuC-mediated) | 🔶 Stub | Needs real `tgl_guc_70.bin` |
| GuC firmware DMA | 🔶 Stub | Disabled until firmware embedded |
| IOAccelerator | ✅ Compilable | Properties published, UserClient attaches |
| Metal integration | ❌ Not started | Requires Apple private framework reverse-eng |

---

## Quick Start

### 1. Prerequisites

- Xcode installed (App Store or developer.apple.com)
- macOS Sequoia 15 SDK (comes with Xcode 16+)
- SIP **disabled**: boot into Recovery → `csrutil disable`
- Boot args in OpenCore `config.plist → NVRAM → boot-args`:
  ```
  amfi_get_out_of_my_way=1 -lilubetaall -wegbeta keepsyms=1 debug=0x100
  ```

### 2. Build

```bash
cd FakeIrisXEFramebuffer
chmod +x build.sh
./build.sh
```

Build output: `build/Debug/FakeIrisXEFramebuffer.kext`

### 3. Install via OpenCore (recommended)

```bash
./build.sh install   # copies to /Volumes/EFI/EFI/OC/Kexts/
```

Then add to `config.plist → Kernel → Add`:

```xml
<dict>
    <key>Arch</key>        <string>x86_64</string>
    <key>BundlePath</key>  <string>FakeIrisXEFramebuffer.kext</string>
    <key>Comment</key>     <string>Tiger Lake Iris Xe GPU driver</string>
    <key>Enabled</key>     <true/>
    <key>ExecutablePath</key> <string>Contents/MacOS/FakeIrisXEFramebuffer</string>
    <key>MaxKernel</key>   <string></string>
    <key>MinKernel</key>   <string>24.0.0</string>
    <key>PlistPath</key>   <string>Contents/Info.plist</string>
</dict>
```

### 4. Load for live iteration (no reboot)

```bash
./build.sh load
```

### 5. Watch the log

```bash
log stream --predicate 'sender == "kernel"' --level debug \
  | grep -E "FakeIrisXE|GuC|Execlist|FORCEWAKE|LRC|ELSP|CSB"
```

---

## Enabling GuC Firmware (Path A)

When the GuC firmware is available, GPU command submission will use Intel's
microkernel scheduler instead of legacy direct-ELSP.

1. Clone linux-firmware:
   ```bash
   git clone https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git
   ```
2. Convert to C array:
   ```bash
   cd linux-firmware
   xxd -i i915/tgl_guc_70.bin > /path/to/project/guc_fw_raw.cpp
   ```
3. Edit `embedded_firmware.cpp`: replace the placeholder arrays with the
   `xxd` output. Rename the array/length to:
   - `gTGLGuCFirmwareData`
   - `gTGLGuCFirmwareSize`
4. In `embedded_firmware.h`, set: `#define GUC_FIRMWARE_AVAILABLE 1`
5. Rebuild.

---

## File Map

```
FakeIrisXEFramebuffer/
├── FakeIrisXEFramebuffer.cpp/hpp  — Main IOFramebuffer + GT init + display pipeline
├── FakeIrisXEGuC.cpp/hpp          — GuC firmware DMA + CTB rings
├── FakeIrisXEExeclist.cpp/hpp     — LRC + ELSP command submission
├── FakeIrisXEGEM.cpp/hpp          — GGTT object allocator
├── FakeIrisXEAccelerator.cpp/hpp  — IOAccelerator stub
├── stubs.cpp                      — Stub impls: AccelDevice, AccelContext,
│                                    UserClients, Backlight
├── embedded_firmware.cpp/h        — GuC firmware placeholder
├── i915_reg.h                     — TGL MMIO register definitions
├── FakeIrisXERing.h               — Ring buffer write helpers
├── FakeIrisXEAccelShared.h        — Shared kernel/userspace types
├── build.sh                       — Build + install + load script
└── FakeIrisXEFramebuffer.xcodeproj/
    └── project.pbxproj
```

---

## Known Compile-time Issues to Resolve

If you hit compile errors, the most likely causes:

1. **`timingCEA861_1920x1080p60` undeclared**: This timing constant lives in
   `IOGraphicsTypes.h`. If missing from your SDK version, substitute `(IOAppleTimingID)2` 
   or declare it manually as `enum { timingCEA861_1920x1080p60 = 0x57 };`

2. **`OSSafeReleaseNULL` missing**: Some older SDK headers don't have it.
   Add to a header: `#define OSSafeReleaseNULL(x) do { if (x) { (x)->release(); (x) = nullptr; } } while(0)`

3. **`IOBufferMemoryDescriptor::inTaskWithOptions` signature**: On some SDK
   versions the `alignment` parameter is positional. If you see argument count
   errors, drop the last `PAGE_SIZE` argument.

4. **`-mkernel` + `libstdc++`**: The `CLANG_CXX_LIBRARY = libstdc++` setting
   is correct for kernel extensions. Don't change it to `libc++`.

---

## References

- Linux i915 driver: https://github.com/torvalds/linux/tree/master/drivers/gpu/drm/i915
- Intel PRM Vol 2c (Display): https://01.org/linuxgraphics/documentation
- Pawan295 upstream work: https://github.com/pawan295/Appleinteltgldriver.kext
- InsanelymMac thread: https://www.insanelymac.com/forum/topic/358305/
- OpenCore config reference: https://dortania.github.io/OpenCore-Install-Guide/
