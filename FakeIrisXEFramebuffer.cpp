/* FakeIrisXEFramebuffer.cpp
 * Main IOFramebuffer subclass — GT power-up, display pipeline, MMIO engine
 */

#include "FakeIrisXEFramebuffer.hpp"
#include "FakeIrisXEGuC.hpp"
#include "FakeIrisXEExeclist.hpp"
#include "FakeIrisXEGEM.hpp"
#include "i915_reg.h"

#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/OSByteOrder.h>

/* -----------------------------------------------------------------------
 * OSDefineMetaClassAndStructors
 * --------------------------------------------------------------------- */
OSDefineMetaClassAndStructors(FakeIrisXEFramebuffer, IOFramebuffer)
/* Explicit parent alias — avoids Tahoe SDK `using super = OSAction` pollution */
#define KEXT_SUPER IOFramebuffer

/* -----------------------------------------------------------------------
 * Logging
 * --------------------------------------------------------------------- */
#define LOG(fmt, ...) IOLog("FakeIrisXEFramebuffer: " fmt "\n", ##__VA_ARGS__)
#define ERR(fmt, ...) IOLog("FakeIrisXEFramebuffer ERROR: " fmt "\n", ##__VA_ARGS__)

/* -----------------------------------------------------------------------
 * Display mode table — single mode: 1920×1080 @ 60 Hz
 * --------------------------------------------------------------------- */
static const IODisplayModeID kModeID_1080p = 1;

/* ═══════════════════════════════════════════════════════════════════════
 * IOService lifecycle
 * ═══════════════════════════════════════════════════════════════════════ */

bool FakeIrisXEFramebuffer::init(OSDictionary *dict) {
    if (!KEXT_SUPER::init(dict)) return false;
    LOG("init");
    return true;
}

bool FakeIrisXEFramebuffer::start(IOService *provider) {
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

    /* CD clock diagnostic — publish to IORegistry so it survives log silence */
    {
        uint32_t cdclk  = mmioRead32(0x46000);
        uint32_t dssm   = mmioRead32(0x51004);
        uint32_t cdfreq = cdclk & 0x7FF;
        uint32_t refclk = (dssm >> 29) & 0x7;
        setProperty("FXE-CDCLK_CTL",  (uint64_t)cdclk,  32);
        setProperty("FXE-DSSM",       (uint64_t)dssm,   32);
        setProperty("FXE-CDFreqField", (uint64_t)cdfreq, 16);
        setProperty("FXE-CDClkOK",    cdfreq >= 0x50E);
        setProperty("FXE-RefClkIdx",  (uint64_t)refclk,  8);
    }

    /* Reprogram CD clock if below 648 MHz — safe to do in start() */
    {
        uint32_t cdfreq = mmioRead32(0x46000) & 0x7FF;
        if (cdfreq < 0x50E) {
            uint32_t pll = mmioRead32(0x46070);
            mmioWrite32(0x46070, pll & ~(1u << 31));
            for (int t = 0; t < 500 && (mmioRead32(0x46070) & (1u<<30)); t++) IODelay(10);
            pll = (mmioRead32(0x46070) & ~0xFF) | 34;
            mmioWrite32(0x46070, pll);
            mmioWrite32(0x46070, pll | (1u << 31));
            for (int t = 0; t < 5000 && !(mmioRead32(0x46070) & (1u<<30)); t++) IODelay(10);
            uint32_t ctl = mmioRead32(0x46000);
            mmioWrite32(0x46000, (ctl & ~0x7FF) | 0x518);
            OSSynchronizeIO();
            uint32_t after = mmioRead32(0x46000) & 0x7FF;
            setProperty("FXE-CDFreqAfter", (uint64_t)after, 16);
            setProperty("FXE-PLLAfter",    (uint64_t)mmioRead32(0x46070), 32);
        }
    }
    LOG("start complete — enableController() called by WindowServer or fxe_test");
    return true;
}



void FakeIrisXEFramebuffer::stop(IOService *provider) {
    LOG("stop");

    /* Stop NDRV hijack state */
    _flipRunning = false;

    /* Tear down subsystems */
    if (_execlist) { _execlist->teardown(); OSSafeReleaseNULL(_execlist); }
    if (_guc)      { _guc->unload();        OSSafeReleaseNULL(_guc); }
    if (_gem)      { _gem->free();          OSSafeReleaseNULL(_gem); }

    forcewakeRelease();

    if (_fbMemDesc) {
        _fbMemDesc->complete();
        OSSafeReleaseNULL(_fbMemDesc);
    }

    if (_mmioMap)   { _mmioMap->unmap(); OSSafeReleaseNULL(_mmioMap); }
    if (_pciDevice) { OSSafeReleaseNULL(_pciDevice); }

    KEXT_SUPER::stop(provider);
}

void FakeIrisXEFramebuffer::free() {
    KEXT_SUPER::free();
}

/* ═══════════════════════════════════════════════════════════════════════
 * MMIO
 * ═══════════════════════════════════════════════════════════════════════ */

IOReturn FakeIrisXEFramebuffer::mapMMIO() {
    /* BAR0 is the primary MMIO aperture on Intel iGPUs */
    IOMemoryDescriptor *barDesc = _pciDevice->getDeviceMemoryWithIndex(0);
    if (!barDesc) {
        ERR("BAR0 not found");
        return kIOReturnNotFound;
    }

    _mmioMap = barDesc->map();
    if (!_mmioMap) {
        ERR("BAR0 map() failed");
        return kIOReturnNoMemory;
    }

    _mmioBase = reinterpret_cast<volatile uint8_t *>(_mmioMap->getVirtualAddress());
    _mmioSize = _mmioMap->getLength();
    LOG("BAR0 mapped: virt=0x%llx size=0x%zx", (uint64_t)_mmioBase, _mmioSize);
    return kIOReturnSuccess;
}

uint32_t FakeIrisXEFramebuffer::mmioRead32(uint32_t offset) {
    return OSReadLittleInt32(_mmioBase, offset);
}

void FakeIrisXEFramebuffer::mmioWrite32(uint32_t offset, uint32_t value) {
    OSWriteLittleInt32(_mmioBase, offset, value);
    OSSynchronizeIO();
}

void FakeIrisXEFramebuffer::mmioWrite64(uint32_t offset, uint64_t value) {
    OSWriteLittleInt32(_mmioBase, offset,     (uint32_t)(value & 0xFFFFFFFF));
    OSWriteLittleInt32(_mmioBase, offset + 4, (uint32_t)(value >> 32));
    OSSynchronizeIO();
}

uint32_t FakeIrisXEFramebuffer::waitBits(uint32_t offset, uint32_t mask,
                                          uint32_t expected, uint32_t timeoutMs) {
    uint32_t deadline = timeoutMs * 10; /* rough loop count */
    uint32_t val;
    do {
        val = mmioRead32(offset);
        if ((val & mask) == expected) return val;
        IODelay(100); /* 100 µs */
    } while (--deadline);
    return val;
}

/* ═══════════════════════════════════════════════════════════════════════
 * FORCEWAKE — keep GT awake during register access sequences
 * ═══════════════════════════════════════════════════════════════════════ */

IOReturn FakeIrisXEFramebuffer::forcewakeGet() {
    if (_forcewakeHeld) return kIOReturnSuccess;

    /* Write WAKE to render forcewake domain */
    mmioWrite32(FORCEWAKE_RENDER_GEN9, FORCEWAKE_KERNEL | (FORCEWAKE_KERNEL << 16));

    /* Wait for ACK */
    uint32_t val = waitBits(FORCEWAKE_ACK_RENDER_GEN9,
                            FORCEWAKE_KERNEL, FORCEWAKE_KERNEL,
                            FORCEWAKE_ACK_TIMEOUT_MS);
    if (!(val & FORCEWAKE_KERNEL)) {
        ERR("FORCEWAKE_RENDER ACK timeout (val=0x%08x)", val);
        return kIOReturnTimeout;
    }

    /* Also wake GT domain */
    mmioWrite32(FORCEWAKE_GT_GEN9, FORCEWAKE_KERNEL | (FORCEWAKE_KERNEL << 16));
    waitBits(FORCEWAKE_ACK_GT_GEN9, FORCEWAKE_KERNEL, FORCEWAKE_KERNEL,
             FORCEWAKE_ACK_TIMEOUT_MS);

    _forcewakeHeld = true;
    LOG("FORCEWAKE held — GT awake");
    return kIOReturnSuccess;
}

void FakeIrisXEFramebuffer::forcewakeRelease() {
    if (!_forcewakeHeld) return;
    /* Write SLEEP — bit 16 = mask bit, bit 0 = value (0 = sleep) */
    mmioWrite32(FORCEWAKE_RENDER_GEN9, (FORCEWAKE_KERNEL << 16));
    mmioWrite32(FORCEWAKE_GT_GEN9,     (FORCEWAKE_KERNEL << 16));
    _forcewakeHeld = false;
    LOG("FORCEWAKE released");
}

/* ═══════════════════════════════════════════════════════════════════════
 * GT initialisation
 * ═══════════════════════════════════════════════════════════════════════ */

IOReturn FakeIrisXEFramebuffer::initGT() {
    IOReturn ret;

    ret = forcewakeGet();
    if (ret != kIOReturnSuccess) return ret;

    ret = enablePowerWells();
    if (ret != kIOReturnSuccess) return ret;

    _gtAwake = true;
    LOG("GT alive");
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEFramebuffer::enablePowerWells() {
    /* TGL uses the ICL+ PACKED power well register layout.
     * ALL wells share ONE driver register at 0x45404 (HSW_PWR_WELL_CTL2).
     * Each well gets 2 bits: REQ = bit(2*idx+1), STATE = bit(2*idx).
     *
     * HOWEVER: when the device-id is spoofed to ICL (0x8A52), the ICLLP
     * framebuffer or WhateverGreen may have already enabled the power wells
     * through their own path. The CTL2 register at 0x45404 reads 0xFF
     * (only low 8 bits respond) — possibly locked or shadowed.
     *
     * BYPASS: If SKL_FUSE_STATUS shows PG1+PG2 fuse distribution is complete,
     * the power gates are already up and we can skip the register writes.
     * The fuse bits are read-only hardware status — they can't lie. */

    /* Check fuse distribution first — are power wells already on? */
    uint32_t fuse = mmioRead32(SKL_FUSE_STATUS);
    bool pg1_dist = (fuse & SKL_FUSE_PG_DIST_STATUS(1)) != 0; /* bit 26 */
    bool pg2_dist = (fuse & SKL_FUSE_PG_DIST_STATUS(2)) != 0; /* bit 25 */
    setProperty("FXE-FUSE-STATUS", (uint64_t)fuse, 32);
    setProperty("FXE-FUSE-PG1-DIST", pg1_dist);
    setProperty("FXE-FUSE-PG2-DIST", pg2_dist);

    if (pg1_dist && pg2_dist) {
        LOG("enablePowerWells: FUSE shows PG1+PG2 already distributed (0x%08x) — skipping register writes", fuse);
        setProperty("FXE-PW-BYPASS", true);
        setProperty("FXE-DC-OFF-OK", true);
        setProperty("FXE-PW1-OK", true);
        setProperty("FXE-PW2-OK", true);

        /* Still do PCI D0 + GT/PUNIT ungating + MBUS/clocks */
        _pciDevice->enablePCIPowerManagement(kPCIPMCSPowerStateD0);
        _pciDevice->setBusMasterEnable(true);
        _pciDevice->setMemoryEnable(true);
        uint16_t pmcsr = _pciDevice->configRead16(0x84);
        _pciDevice->configWrite16(0x84, pmcsr & ~0x3);
        IODelay(10000);

        mmioWrite32(0xA218, mmioRead32(0xA218) & ~0x1);
        IODelay(10000);
        mmioWrite32(0xA2B0, mmioRead32(0xA2B0) & ~0x80000000);
        IODelay(15000);

        /* DC states — ensure disabled */
        mmioWrite32(DC_STATE_EN, DC_STATE_DISABLE);
        IODelay(1000);

        /* MBUS + display clocks */
        mmioWrite32(0x7003C, 0xb1038c02);
        IODelay(10000);
        mmioWrite32(0x46010, 0xcc000000);
        IODelay(10000);
        mmioWrite32(0x46140, 0x10000000);
        IODelay(10000);

        setProperty("FXE-PW-CTL2-FINAL", (uint64_t)mmioRead32(0x45404), 32);
        setProperty("FXE-DC-STATE-FINAL", (uint64_t)mmioRead32(DC_STATE_EN), 32);
        LOG("enablePowerWells complete via fuse bypass — wells already up");
        return kIOReturnSuccess;
    }

    LOG("enablePowerWells: FUSE PG1=%d PG2=%d — attempting register-based enable",
        pg1_dist, pg2_dist);
    setProperty("FXE-PW-BYPASS", false);

    const uint32_t CTL2 = 0x45404;  /* THE packed driver register */

    /* Bit positions: REQ=bit(2*idx+1), STATE=bit(2*idx) */
    const uint32_t DC_OFF_REQ = (1u << (2 * TGL_PW_CTL_IDX_DC_OFF + 1)); /* bit19 */
    const uint32_t DC_OFF_STA = (1u << (2 * TGL_PW_CTL_IDX_DC_OFF));     /* bit18 */
    const uint32_t PW1_REQ    = (1u << (2 * TGL_PW_CTL_IDX_PW_1 + 1));   /* bit29 */
    const uint32_t PW1_STA    = (1u << (2 * TGL_PW_CTL_IDX_PW_1));       /* bit28 */
    const uint32_t PW2_REQ    = (1u << (2 * TGL_PW_CTL_IDX_PW_2 + 1));   /* bit27 */
    const uint32_t PW2_STA    = (1u << (2 * TGL_PW_CTL_IDX_PW_2));       /* bit26 */

    /* Step 0: Force PCI to D0 */
    _pciDevice->enablePCIPowerManagement(kPCIPMCSPowerStateD0);
    _pciDevice->setBusMasterEnable(true);
    _pciDevice->setMemoryEnable(true);
    uint16_t pmcsr = _pciDevice->configRead16(0x84);
    _pciDevice->configWrite16(0x84, pmcsr & ~0x3);
    IODelay(10000);

    /* Disable GT + PUNIT power gating */
    mmioWrite32(0xA218, mmioRead32(0xA218) & ~0x1);
    IODelay(10000);
    mmioWrite32(0xA2B0, mmioRead32(0xA2B0) & ~0x80000000);
    IODelay(15000);

    /* Step 1: Disable DC states */
    mmioWrite32(DC_STATE_EN, DC_STATE_DISABLE);
    IODelay(1000);
    setProperty("FXE-DC-STATE-AFTER", (uint64_t)mmioRead32(DC_STATE_EN), 32);
    LOG("enablePowerWells: DC states disabled (0x45504=0x%08x)",
        mmioRead32(DC_STATE_EN));

    uint32_t ctl2_initial = mmioRead32(CTL2);
    setProperty("FXE-PW-CTL2-INITIAL", (uint64_t)ctl2_initial, 32);
    LOG("enablePowerWells: CTL2 initial=0x%08x", ctl2_initial);

    /* Step 2: Enable DC_OFF (idx=9) */
    {
        mmioWrite32(CTL2, mmioRead32(CTL2) | DC_OFF_REQ);
        OSSynchronizeIO();
        bool ok = false;
        for (int t = 0; t < 500; t++) {
            if (mmioRead32(CTL2) & DC_OFF_STA) { ok = true; break; }
            IODelay(100);
        }
        uint32_t after = mmioRead32(CTL2);
        setProperty("FXE-DC-OFF-OK", ok);
        setProperty("FXE-PW-CTL2-DCOFF", (uint64_t)after, 32);
        LOG("enablePowerWells: DC_OFF %s (CTL2=0x%08x)", ok ? "OK" : "TIMEOUT", after);
        if (!ok) {
            ERR("DC_OFF timeout");
            return kIOReturnTimeout;
        }
    }

    /* Step 3: Enable PW1 (idx=14) */
    {
        mmioWrite32(CTL2, mmioRead32(CTL2) | PW1_REQ);
        OSSynchronizeIO();
        bool ok = false;
        for (int t = 0; t < 500; t++) {
            if (mmioRead32(CTL2) & PW1_STA) { ok = true; break; }
            IODelay(100);
        }
        uint32_t after = mmioRead32(CTL2);
        setProperty("FXE-PW1-OK", ok);
        setProperty("FXE-PW-CTL2-PW1", (uint64_t)after, 32);
        LOG("enablePowerWells: PW1 %s (CTL2=0x%08x)", ok ? "OK" : "TIMEOUT", after);
        if (!ok) {
            ERR("PW1 timeout (CTL2=0x%08x)", after);
            return kIOReturnTimeout;
        }
    }

    /* Fuse distribution PG1 */
    {
        bool fuse_ok = false;
        for (int t = 0; t < 100; t++) {
            if (mmioRead32(SKL_FUSE_STATUS) & SKL_FUSE_PG_DIST_STATUS(1)) { fuse_ok = true; break; }
            IODelay(10);
        }
        setProperty("FXE-FUSE-PG1", fuse_ok);
        setProperty("FXE-FUSE-STATUS", (uint64_t)mmioRead32(SKL_FUSE_STATUS), 32);
        if (!fuse_ok) LOG("enablePowerWells: FUSE PG1 dist not set (non-fatal)");
    }

    /* Step 4: Enable PW2 (idx=13) */
    {
        mmioWrite32(CTL2, mmioRead32(CTL2) | PW2_REQ);
        OSSynchronizeIO();
        bool ok = false;
        for (int t = 0; t < 500; t++) {
            if (mmioRead32(CTL2) & PW2_STA) { ok = true; break; }
            IODelay(100);
        }
        uint32_t after = mmioRead32(CTL2);
        setProperty("FXE-PW2-OK", ok);
        setProperty("FXE-PW-CTL2-PW2", (uint64_t)after, 32);
        LOG("enablePowerWells: PW2 %s (CTL2=0x%08x)", ok ? "OK" : "TIMEOUT", after);
        if (!ok) {
            ERR("PW2 timeout (CTL2=0x%08x)", after);
            return kIOReturnTimeout;
        }
    }

    /* Fuse distribution PG2 */
    {
        bool fuse_ok = false;
        for (int t = 0; t < 100; t++) {
            if (mmioRead32(SKL_FUSE_STATUS) & SKL_FUSE_PG_DIST_STATUS(2)) { fuse_ok = true; break; }
            IODelay(10);
        }
        setProperty("FXE-FUSE-PG2", fuse_ok);
    }

    /* Step 5: MBUS + display clocks */
    mmioWrite32(0x7003C, 0xb1038c02);
    IODelay(10000);
    mmioWrite32(0x46010, 0xcc000000);
    IODelay(10000);
    mmioWrite32(0x46140, 0x10000000);
    IODelay(10000);

    /* Final state */
    setProperty("FXE-PW-CTL2-FINAL", (uint64_t)mmioRead32(CTL2), 32);
    setProperty("FXE-FUSE-FINAL",    (uint64_t)mmioRead32(SKL_FUSE_STATUS), 32);
    setProperty("FXE-DC-STATE-FINAL", (uint64_t)mmioRead32(DC_STATE_EN), 32);

    LOG("enablePowerWells complete — DC_OFF+PW1+PW2 up (CTL2=0x%08x)", mmioRead32(CTL2));
    return kIOReturnSuccess;
}

/* ═══════════════════════════════════════════════════════════════════════
 * Framebuffer allocation (system DRAM, no stolen needed for stub)
 * ═══════════════════════════════════════════════════════════════════════ */

IOReturn FakeIrisXEFramebuffer::allocateFramebuffer() {
    _fbSize = kDisplayWidth * kDisplayHeight * kBytesPerPixel;

    _fbMemDesc = IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task,
        kIOMemoryPhysicallyContiguous | kIODirectionInOut,
        _fbSize,
        PAGE_SIZE);

    if (!_fbMemDesc) {
        ERR("could not allocate %zu bytes for framebuffer", _fbSize);
        return kIOReturnNoMemory;
    }

    if (_fbMemDesc->prepare() != kIOReturnSuccess) {
        ERR("framebuffer IOMemoryDescriptor::prepare() failed");
        OSSafeReleaseNULL(_fbMemDesc);
        return kIOReturnNoMemory;
    }

    _fbPhysAddr = _fbMemDesc->getPhysicalAddress();
    _fbVirtAddr = _fbMemDesc->getBytesNoCopy();

    /* Clear to black */
    memset(_fbVirtAddr, 0x00, _fbSize);

    LOG("framebuffer allocated: phys=0x%llx virt=%p size=%zu",
        (uint64_t)_fbPhysAddr, _fbVirtAddr, _fbSize);
    return kIOReturnSuccess;
}

/* ═══════════════════════════════════════════════════════════════════════
 * Display pipeline — Pipe A / Transcoder A / Plane 1A — 1920×1080 @ 60 Hz
 * ═══════════════════════════════════════════════════════════════════════ */

IOReturn FakeIrisXEFramebuffer::initDisplayPipeline() {
    IOReturn ret;

    ret = programPipeA_1080p60();
    if (ret != kIOReturnSuccess) return ret;

    ret = programPlane1A(_fbPhysAddr, kStride);
    if (ret != kIOReturnSuccess) return ret;

    _displayInit = true;
    LOG("display pipeline armed — 1920×1080@60Hz eDP");
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEFramebuffer::programPipeA_1080p60() {
    /* ---------------------------------------------------------------
     * CEA-861 1920×1080p60 timings (pixel clock 148.5 MHz)
     *  Htotal = 2200, Vtotal = 1125
     *  HSync start=2008 end=2052; VSync start=1084 end=1089
     * ------------------------------------------------------------- */
    const uint32_t pipe = 0;  /* Pipe A */
    const uint32_t trans = 0; /* Transcoder A */

    /* Pipe config */
    mmioWrite32(PIPECONF(pipe), 0); /* disable first */
    IODelay(1000);

    /* Transcoder timing */
    /* HTOTAL: [15:0]=hActive-1  [31:16]=hTotal-1 */
    mmioWrite32(TRANS_HTOTAL(trans), ((2200-1) << 16) | (1920-1));
    mmioWrite32(TRANS_HBLANK(trans), ((2200-1) << 16) | (1920-1));
    mmioWrite32(TRANS_HSYNC(trans),  ((2052-1) << 16) | (2008-1));
    mmioWrite32(TRANS_VTOTAL(trans), ((1125-1) << 16) | (1080-1));
    mmioWrite32(TRANS_VBLANK(trans), ((1125-1) << 16) | (1080-1));
    mmioWrite32(TRANS_VSYNC(trans),  ((1089-1) << 16) | (1084-1));

    /* Enable transcoder */
    mmioWrite32(TRANSCONF(trans), TRANS_ENABLE);
    uint32_t val = waitBits(TRANSCONF(trans), TRANS_STATE_ENABLE,
                            TRANS_STATE_ENABLE, 20);
    if (!(val & TRANS_STATE_ENABLE)) {
        ERR("Transcoder A did not enable (TRANSCONF=0x%08x)", val);
        return kIOReturnTimeout;
    }
    LOG("Transcoder A enabled");

    /* Enable pipe */
    mmioWrite32(PIPECONF(pipe), PIPECONF_ENABLE | PIPECONF_8BPC);
    val = waitBits(PIPECONF(pipe), PIPECONF_STATE_ENABLE, PIPECONF_STATE_ENABLE, 20);
    if (!(val & PIPECONF_STATE_ENABLE)) {
        ERR("Pipe A did not enable (PIPECONF=0x%08x)", val);
        return kIOReturnTimeout;
    }
    LOG("Pipe A enabled");
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEFramebuffer::programPlane1A(IOPhysicalAddress fbPhys,
                                                 uint32_t stride) {
    const uint32_t pipe  = 0;
    const uint32_t plane = 0; /* Primary plane */

    /* Universal plane control (TGL ICL+) */
    mmioWrite32(PLANE_CTL(pipe, plane),
                PLANE_CTL_ENABLE          |
                PLANE_CTL_FORMAT_XRGB_8888|
                PLANE_CTL_PIPE_GAMMA_ENABLE|
                PLANE_CTL_PLANE_GAMMA_DISABLE);

    /* Stride in bytes */
    mmioWrite32(PLANE_STRIDE(pipe, plane), stride);

    /* Size: [15:0]=width-1  [31:16]=height-1 */
    mmioWrite32(PLANE_SIZE(pipe, plane),
                ((kDisplayHeight - 1) << 16) | (kDisplayWidth - 1));

    /* Offset within surface — zero for base plane */
    mmioWrite32(PLANE_OFFSET(pipe, plane), 0);

    /* Surface base — writing this commits the plane */
    mmioWrite32(PLANE_SURF(pipe, plane), (uint32_t)(fbPhys & 0xFFFFF000));

    OSSynchronizeIO();
    LOG("Plane 1A programmed: phys=0x%llx stride=%u", (uint64_t)fbPhys, stride);
    return kIOReturnSuccess;
}

/* ═══════════════════════════════════════════════════════════════════════
 * GuC + Execlists system init
 * ═══════════════════════════════════════════════════════════════════════ */

IOReturn FakeIrisXEFramebuffer::initGuCSystem() {
    /* Try GuC first */
    _guc = new FakeIrisXEGuC();
    if (_guc && _guc->init(this)) {
        IOReturn ret = _guc->load();
        if (ret == kIOReturnSuccess) {
            LOG("GuC loaded — using GuC submission");
        } else {
            LOG("GuC load failed (0x%08x) — falling back to Execlists", ret);
            _guc->unload();
            OSSafeReleaseNULL(_guc);
        }
    } else {
        OSSafeReleaseNULL(_guc);
    }

    /* Always init Execlists (it drives context execution regardless of GuC) */
    _execlist = new FakeIrisXEExeclist();
    if (!_execlist || !_execlist->init(this, (_guc != nullptr))) {
        ERR("Execlists init failed");
        OSSafeReleaseNULL(_execlist);
        return kIOReturnError;
    }

    LOG("initGuCSystem complete (GuC=%s execlist=ready)",
        _guc ? "enabled" : "disabled/fallback");
    return kIOReturnSuccess;
}

/* ═══════════════════════════════════════════════════════════════════════
 * IOFramebuffer API
 * ═══════════════════════════════════════════════════════════════════════ */

IOReturn FakeIrisXEFramebuffer::requestProbe(IOOptionBits options) {
    LOG("requestProbe options=0x%08x — running HW test", options);

    /* Run hardware test sequence and publish results to IORegistry */
    uint32_t cdclk_before = _mmioBase ? (mmioRead32(0x46000) & 0x7FF) : 0xDEAD;
    uint32_t dssm         = _mmioBase ? mmioRead32(0x51004) : 0;
    setProperty("FXE-Test-CDClk-Before", (uint64_t)cdclk_before, 16);
    setProperty("FXE-Test-DSSM",         (uint64_t)dssm,         32);

    /* FORCEWAKE */
    IOReturn ret = forcewakeGet();
    setProperty("FXE-Test-ForcewakeOK", ret == kIOReturnSuccess);

    if (ret == kIOReturnSuccess) {
        /* Wake display FORCEWAKE domain — required for display power well registers */
        mmioWrite32(0xA204, 0x00010001);
        for (int t = 0; t < 500; t++) {
            if (mmioRead32(0xD44) & 1) break;
            IODelay(10);
        }
        uint32_t fw_ack = mmioRead32(0xD44);
        setProperty("FXE-FW-MEDIA-ACK", (uint64_t)fw_ack, 32);
        LOG("requestProbe: FORCEWAKE_MEDIA ack=0x%08x", fw_ack);
        /* Power Well 1 — driver register is HSW_PWR_WELL_CTL_DRIVER(pw_idx)
         * = 0x45404 + pw_idx * 8
         * State/req bits within that register are REQ(0) and STATE(0)
         * because each driver register covers one power well */
        /* Use correct bit positions for PW1/PW2 within 0x45404
         * PW1 idx=14: REQ=bit29=0x20000000, STA=bit28=0x10000000
         * PW2 idx=13: REQ=bit27=0x08000000, STA=bit26=0x04000000 */
        uint32_t pw_reg = 0x45404;  /* HSW_PWR_WELL_CTL2 */
        uint32_t pw1req = HSW_PWR_WELL_CTL_REQ(TGL_PW_CTL_IDX_PW_1);  /* 0x20000000 */
        uint32_t pw1sta = HSW_PWR_WELL_CTL_STATE(TGL_PW_CTL_IDX_PW_1); /* 0x10000000 */

        /* Read before write */
        uint32_t before = mmioRead32(pw_reg);
        setProperty("FXE-PW-CTL2-BEFORE", (uint64_t)before, 32);

        /* Write PW1 request */
        mmioWrite32(pw_reg, before | pw1req);
        OSSynchronizeIO();
        uint32_t after_write = mmioRead32(pw_reg);
        setProperty("FXE-PW-CTL2-AFTER-WRITE", (uint64_t)after_write, 32);

        /* Poll for state */
        for (int t = 0; t < 500; t++) {
            if (mmioRead32(pw_reg) & pw1sta) break;
            IODelay(100);
        }
        uint32_t pw1val = mmioRead32(pw_reg);
        bool pw1ok = (pw1val & pw1sta) != 0;
        setProperty("FXE-Test-PW1OK", pw1ok);
        setProperty("FXE-PW-CTL2-AFTER-PW1", (uint64_t)pw1val, 32);
        LOG("requestProbe: PW1 before=0x%08x after_write=0x%08x final=0x%08x %s",
            before, after_write, pw1val, pw1ok ? "ON" : "TIMEOUT");

        /* PW2 */
        uint32_t pw2req = HSW_PWR_WELL_CTL_REQ(TGL_PW_CTL_IDX_PW_2);
        uint32_t pw2sta = HSW_PWR_WELL_CTL_STATE(TGL_PW_CTL_IDX_PW_2);
        mmioWrite32(pw_reg, mmioRead32(pw_reg) | pw2req);
        OSSynchronizeIO();
        for (int t = 0; t < 500; t++) {
            if (mmioRead32(pw_reg) & pw2sta) break;
            IODelay(100);
        }
        uint32_t pw2val = mmioRead32(pw_reg);
        bool pw2ok = (pw2val & pw2sta) != 0;
        setProperty("FXE-Test-PW2OK", pw2ok);
        setProperty("FXE-PW-CTL2-AFTER-PW2", (uint64_t)pw2val, 32);
        LOG("requestProbe: PW2 final=0x%08x %s", pw2val, pw2ok ? "ON" : "TIMEOUT");

        uint32_t cdclk_after = mmioRead32(0x46000) & 0x7FF;
        setProperty("FXE-Test-CDClk-After", (uint64_t)cdclk_after, 16);

        forcewakeRelease();
    }

    /* Power wells via corrected packed-register path */
    if (_mmioBase) {
        setProperty("FXE-PW-CTL1-BIOS",  (uint64_t)mmioRead32(0x45400), 32);
        setProperty("FXE-PW-CTL2-DRV",   (uint64_t)mmioRead32(0x45404), 32);
        setProperty("FXE-FUSE-PRE",      (uint64_t)mmioRead32(SKL_FUSE_STATUS), 32);
        setProperty("FXE-DC-STATE-PRE",  (uint64_t)mmioRead32(DC_STATE_EN), 32);
    }

    IOReturn pwRet = enablePowerWells();
    setProperty("FXE-PW-ENABLE-RET", (uint64_t)pwRet, 32);
    bool pwOK = (pwRet == kIOReturnSuccess);
    setProperty("FXE-SEQ-DONE", pwOK);

    /* Display pipeline — only attempt if power wells are up */
    if (pwOK) do {
            const uint32_t PIPE_SRC_A       = 0x6001C;
            const uint32_t PIPECONF_A       = 0x70008;
            /* Plane 1 — confirmed working (produced red screen in first test).
             * Pawan's TGL driver also uses Plane 1 exclusively.
             * NDRV will eventually overwrite, but we get first frame. */
            const uint32_t PLANE_CTL_1_A    = 0x70180;
            const uint32_t PLANE_SURF_1_A   = 0x7019C;
            const uint32_t PLANE_STRIDE_1_A = 0x70188;
            const uint32_t PLANE_SIZE_1_A   = 0x70190;
            const uint32_t PLANE_POS_1_A    = 0x7018C;

            /* Step A: Allocate framebuffer (32MB) */
            const uint32_t fbSize = 32 * 1024 * 1024;
            const uint32_t fbGGTTOffset = 0x800; /* pawan's proven offset */
            IOBufferMemoryDescriptor* fbMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
                kernel_task,
                kIODirectionInOut | kIOMemoryKernelUserShared,
                fbSize,
                0x000000003FFFF000ULL
            );
            if (!fbMem || fbMem->prepare() != kIOReturnSuccess) {
                ERR("Failed to allocate framebuffer");
                if (fbMem) { fbMem->release(); fbMem = nullptr; }
                setProperty("FXE-FB-ALLOC", false);
                break;
            }
            setProperty("FXE-FB-PHYS", (uint64_t)fbMem->getPhysicalAddress(), 64);
            LOG("hwInitAsync: FB allocated phys=0x%llx", fbMem->getPhysicalAddress());

            /* Step B: Install GGTT PTEs via BAR1 (GTTMMADR)
             * This is the proven working approach — our first test with BAR1
             * produced a visible red screen. Pawan's driver also uses BAR1. */
            {
                uint64_t bar1Lo = _pciDevice->configRead32(0x18) & ~0xFULL;
                uint64_t bar1Hi = _pciDevice->configRead32(0x1C);
                uint64_t gttPhys = (bar1Hi << 32) | bar1Lo;
                setProperty("FXE-GTT-PHYS", gttPhys, 64);
                setProperty("FXE-GTT-METHOD", "BAR1");

                IOMemoryDescriptor* gttDesc = IOMemoryDescriptor::withPhysicalAddress(
                    gttPhys, 0x1000000, kIODirectionInOut);
                if (!gttDesc) {
                    ERR("Failed to create GTT descriptor");
                    fbMem->release();
                    setProperty("FXE-GTT-MAP", false);
                    break;
                }
                IOMemoryMap* gttMap = gttDesc->map();
                if (!gttMap) {
                    gttDesc->release(); fbMem->release();
                    setProperty("FXE-GTT-MAP", false);
                    break;
                }

                volatile uint64_t* ggtt = (volatile uint64_t*)gttMap->getVirtualAddress();
                const uint32_t ggttBaseIndex = fbGGTTOffset >> 12; /* = 0 for offset 0x800 */
                const uint32_t kPageSize = 4096;
                setProperty("FXE-GTT-BASE-INDEX", (uint64_t)ggttBaseIndex, 32);

                /* Install PTEs */
                IOByteCount offset = 0;
                uint32_t page = 0;
                while (offset < fbSize) {
                    IOByteCount segLen = 0;
                    IOPhysicalAddress segPhys = fbMem->getPhysicalSegment(offset, &segLen);
                    if (!segPhys || !segLen) break;
                    segLen &= ~(kPageSize - 1);
                    for (IOByteCount segOff = 0; segOff < segLen && offset < fbSize;
                         segOff += kPageSize, offset += kPageSize, ++page) {
                        uint64_t phys = (uint64_t)(segPhys + segOff);
                        ggtt[ggttBaseIndex + page] = (phys & ~0xFFFULL) | 0x3;
                    }
                }
                OSSynchronizeIO();

                /* Verify first PTE */
                uint64_t pte0 = ggtt[ggttBaseIndex];
                setProperty("FXE-GTT-PTE0", pte0, 64);
                setProperty("FXE-GTT-PAGES", (uint64_t)page, 32);
                LOG("hwInitAsync: %u GGTT PTEs at index %u (PTE0=0x%llx)", page, ggttBaseIndex, pte0);

                gttMap->release();
                gttDesc->release();
            }

            /* Step C: Read BIOS resolution and fill framebuffer GREEN */
            uint32_t pipeSrc = mmioRead32(PIPE_SRC_A);
            setProperty("FXE-PIPE-SRC-BIOS", (uint64_t)pipeSrc, 32);
            uint32_t dispW = ((pipeSrc >> 16) & 0xFFFF) + 1;
            uint32_t dispH = (pipeSrc & 0xFFFF) + 1;
            if (dispW < 640 || dispW > 3840) dispW = 1920;
            if (dispH < 480 || dispH > 2400) dispH = 1200;
            setProperty("FXE-REAL-WIDTH",  (uint64_t)dispW, 32);
            setProperty("FXE-REAL-HEIGHT", (uint64_t)dispH, 32);

            {
                uint32_t* pixels = (uint32_t*)fbMem->getBytesNoCopy();
                if (pixels) {
                    uint32_t count = dispW * dispH;
                    if (count > fbSize / 4) count = fbSize / 4;
                    for (uint32_t i = 0; i < count; i++) pixels[i] = 0x0000FF00;
                    setProperty("FXE-FILL-PIXELS", (uint64_t)count, 32);
                }
            }

            /* Step D: Program Plane 1 geometry */
            uint32_t stride = (dispW * 4) / 64;
            mmioWrite32(PLANE_POS_1_A,  0x00000000);
            mmioWrite32(PLANE_SIZE_1_A, ((dispH-1)<<16)|(dispW-1));
            mmioWrite32(PLANE_STRIDE_1_A, stride);

            /* Pipe-level watermarks (proven by pawan's driver) */
            mmioWrite32(0xC4060, 0x00003FFF);
            mmioWrite32(0xC4064, 0x00000010);
            mmioWrite32(0xC4068, 0x00000020);
            mmioWrite32(0xC406C, 0x00000040);
            mmioWrite32(0xC4070, 0x00000080);
            mmioWrite32(0xC4020, 0x0000000F);

            /* Step E: Disable plane, write surface, re-enable */
            mmioWrite32(PLANE_CTL_1_A, mmioRead32(PLANE_CTL_1_A) & ~(1u<<31));
            (void)mmioRead32(PLANE_SURF_1_A);
            IOSleep(2);

            mmioWrite32(PLANE_SURF_1_A, fbGGTTOffset);
            mmioWrite32(PLANE_STRIDE_1_A, stride);

            /* PLANE_CTL: enable(31) | XRGB8888(4<<24) | pipe gamma(22) | PipeA(3) */
            uint32_t planeCtl = (1u<<31) | (4u<<24) | (1u<<22) | (1u<<3);
            mmioWrite32(PLANE_CTL_1_A, planeCtl);
            mmioWrite32(PLANE_SURF_1_A, fbGGTTOffset); /* trigger flip */
            (void)mmioRead32(PLANE_SURF_1_A);

            setProperty("FXE-PLANE-CTL-FINAL",  (uint64_t)mmioRead32(PLANE_CTL_1_A), 32);
            setProperty("FXE-PLANE-SURF-FINAL", (uint64_t)mmioRead32(PLANE_SURF_1_A), 32);
            setProperty("FXE-PIPECONF-FINAL",   (uint64_t)mmioRead32(PIPECONF_A), 32);

            setProperty("FXE-DISPLAY-INIT", true);
            LOG("hwInitAsync: Plane 1 armed with green fill — screen should show green");

        } while (0); /* display pipeline block */
    setProperty("FXE-Test-Done", true);
    LOG("requestProbe: HW test complete");
    return kIOReturnSuccess;
}


IOReturn FakeIrisXEFramebuffer::enableController() {
    /* Run display pipeline init ONCE, before WindowServer starts.
     * This is the window where NDRV hasn't begun flipping Plane 1 yet.
     * Pawan's TGL driver uses the same approach. */
    static bool hasRun = false;
    if (hasRun) {
        LOG("enableController: already ran — skipping");
        return kIOReturnSuccess;
    }
    hasRun = true;
    LOG("enableController: running display pipeline init (first call only)");

    if (!_mmioBase || !_pciDevice) {
        ERR("enableController: MMIO or PCI not ready");
        return kIOReturnNotReady;
    }

    /* Power wells via fuse bypass */
    IOReturn pwRet = enablePowerWells();
    setProperty("FXE-EC-PW-RET", (uint64_t)pwRet, 32);
    if (pwRet != kIOReturnSuccess) {
        ERR("enableController: power wells failed");
        return pwRet;
    }

    /* Display pipeline — Plane 1, Pipe A */
    do {
        const uint32_t PIPE_SRC_A       = 0x6001C;
        const uint32_t PIPECONF_A       = 0x70008;
        const uint32_t PLANE_CTL_1_A    = 0x70180;
        const uint32_t PLANE_SURF_1_A   = 0x7019C;
        const uint32_t PLANE_STRIDE_1_A = 0x70188;
        const uint32_t PLANE_SIZE_1_A   = 0x70190;
        const uint32_t PLANE_POS_1_A    = 0x7018C;

        /* Allocate framebuffer (32MB) */
        const uint32_t fbSize = 32 * 1024 * 1024;
        const uint32_t fbGGTTOffset = 0x800;
        IOBufferMemoryDescriptor* fbMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task,
            kIODirectionInOut | kIOMemoryKernelUserShared,
            fbSize,
            0x000000003FFFF000ULL
        );
        if (!fbMem || fbMem->prepare() != kIOReturnSuccess) {
            ERR("enableController: FB alloc failed");
            if (fbMem) { fbMem->release(); }
            break;
        }
        setProperty("FXE-EC-FB-PHYS", (uint64_t)fbMem->getPhysicalAddress(), 64);

        /* GGTT PTEs via BAR1 */
        {
            uint64_t bar1Lo = _pciDevice->configRead32(0x18) & ~0xFULL;
            uint64_t bar1Hi = _pciDevice->configRead32(0x1C);
            uint64_t gttPhys = (bar1Hi << 32) | bar1Lo;

            IOMemoryDescriptor* gttDesc = IOMemoryDescriptor::withPhysicalAddress(
                gttPhys, 0x1000000, kIODirectionInOut);
            if (!gttDesc) { fbMem->release(); break; }
            IOMemoryMap* gttMap = gttDesc->map();
            if (!gttMap) { gttDesc->release(); fbMem->release(); break; }

            volatile uint64_t* ggtt = (volatile uint64_t*)gttMap->getVirtualAddress();
            const uint32_t ggttBaseIndex = fbGGTTOffset >> 12;
            const uint32_t kPageSize = 4096;

            IOByteCount offset = 0;
            uint32_t page = 0;
            while (offset < fbSize) {
                IOByteCount segLen = 0;
                IOPhysicalAddress segPhys = fbMem->getPhysicalSegment(offset, &segLen);
                if (!segPhys || !segLen) break;
                segLen &= ~(kPageSize - 1);
                for (IOByteCount segOff = 0; segOff < segLen && offset < fbSize;
                     segOff += kPageSize, offset += kPageSize, ++page) {
                    uint64_t phys = (uint64_t)(segPhys + segOff);
                    ggtt[ggttBaseIndex + page] = (phys & ~0xFFFULL) | 0x3;
                }
            }
            OSSynchronizeIO();
            setProperty("FXE-EC-GTT-PAGES", (uint64_t)page, 32);

            gttMap->release();
            gttDesc->release();
        }

        /* Read BIOS resolution, fill green */
        uint32_t pipeSrc = mmioRead32(PIPE_SRC_A);
        uint32_t dispW = ((pipeSrc >> 16) & 0xFFFF) + 1;
        uint32_t dispH = (pipeSrc & 0xFFFF) + 1;
        if (dispW < 640 || dispW > 3840) dispW = 1920;
        if (dispH < 480 || dispH > 2400) dispH = 1200;
        setProperty("FXE-EC-WIDTH", (uint64_t)dispW, 32);
        setProperty("FXE-EC-HEIGHT", (uint64_t)dispH, 32);

        {
            uint32_t* pixels = (uint32_t*)fbMem->getBytesNoCopy();
            if (pixels) {
                uint32_t count = dispW * dispH;
                if (count > fbSize / 4) count = fbSize / 4;
                for (uint32_t i = 0; i < count; i++) pixels[i] = 0x0000FF00;
            }
        }

        /* Program Plane 1 */
        uint32_t stride = (dispW * 4) / 64;
        mmioWrite32(PLANE_POS_1_A, 0x00000000);
        mmioWrite32(PLANE_SIZE_1_A, ((dispH-1)<<16)|(dispW-1));
        mmioWrite32(PLANE_STRIDE_1_A, stride);

        /* Pipe watermarks */
        mmioWrite32(0xC4060, 0x00003FFF);
        mmioWrite32(0xC4064, 0x00000010);
        mmioWrite32(0xC4068, 0x00000020);
        mmioWrite32(0xC406C, 0x00000040);
        mmioWrite32(0xC4070, 0x00000080);
        mmioWrite32(0xC4020, 0x0000000F);

        /* Disable, program surface, re-enable */
        mmioWrite32(PLANE_CTL_1_A, mmioRead32(PLANE_CTL_1_A) & ~(1u<<31));
        (void)mmioRead32(PLANE_SURF_1_A);
        IOSleep(2);

        mmioWrite32(PLANE_SURF_1_A, fbGGTTOffset);
        mmioWrite32(PLANE_STRIDE_1_A, stride);

        uint32_t planeCtl = (1u<<31) | (4u<<24) | (1u<<22) | (1u<<3);
        mmioWrite32(PLANE_CTL_1_A, planeCtl);
        mmioWrite32(PLANE_SURF_1_A, fbGGTTOffset); /* trigger flip */
        (void)mmioRead32(PLANE_SURF_1_A);

        setProperty("FXE-EC-PLANE-CTL", (uint64_t)mmioRead32(PLANE_CTL_1_A), 32);
        setProperty("FXE-EC-PLANE-SURF", (uint64_t)mmioRead32(PLANE_SURF_1_A), 32);
        setProperty("FXE-EC-DONE", true);
        LOG("enableController: Plane 1 armed with green fill");

    } while (0);

    return kIOReturnSuccess;
}
IOItemCount FakeIrisXEFramebuffer::getConnectionCount() {
    return 1;
}

IOReturn FakeIrisXEFramebuffer::getAttributeForConnection(IOIndex connectIndex,
                                                            IOSelect attribute,
                                                            uintptr_t *value) {
    if (!value) return kIOReturnBadArgument;

    switch (attribute) {
        case kConnectionSupportsHLDDCSense:
        case kConnectionSupportsLLDDCSense:
        /* kConnectionSupportsMonitorEdidAccess removed from Tahoe SDK — use raw value */
        case (IOSelect)0x00000406:
            *value = 1;
            return kIOReturnSuccess;
        default:
            return IOFramebuffer::getAttributeForConnection(connectIndex, attribute, value);
    }
}

IOReturn FakeIrisXEFramebuffer::setAttributeForConnection(IOIndex connectIndex,
                                                            IOSelect attribute,
                                                            uintptr_t value) {
    return IOFramebuffer::setAttributeForConnection(connectIndex, attribute, value);
}

bool FakeIrisXEFramebuffer::isConsoleDevice() {
    return false;
}

/* getApertureRange — pure virtual. Returns IODeviceMemory for the FB aperture.
 * Uses GPU stolen memory (real VRAM) — IODeviceMemory::withRange on actual
 * device memory maps correctly with kIOMapWriteCombineCache unlike
 * IOBufferMemoryDescriptor-backed pages which deadlock in doSetup(). */
IODeviceMemory * FakeIrisXEFramebuffer::getApertureRange(IOPixelAperture aperture) {
    if (!_pciDevice) return nullptr;
    IOPhysicalAddress stolenBase = getStolenMemBase();
    size_t stolenSize = getStolenMemSize();
    if (!stolenBase || !stolenSize) return nullptr;
    /* Return full stolen memory range as the aperture */
    LOG("getApertureRange aperture=%d stolen=0x%llx len=0x%lx",
        aperture, (uint64_t)stolenBase, (unsigned long)stolenSize);
    return IODeviceMemory::withRange(stolenBase, stolenSize);
}

/* getPixelFormats — pure virtual. Null-separated, double-null-terminated string list. */
const char * FakeIrisXEFramebuffer::getPixelFormats(void) {
    static const char fmt[] = IO32BitDirectPixels "\0";
    return fmt;
}

/* getDisplayModeCount — pure virtual. */
IOItemCount FakeIrisXEFramebuffer::getDisplayModeCount(void) {
    return 1;
}

IOReturn FakeIrisXEFramebuffer::getDisplayModes(IODisplayModeID *allDisplayModes) {
    if (!allDisplayModes) return kIOReturnBadArgument;
    allDisplayModes[0] = kModeID_1080p;
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEFramebuffer::getCurrentDisplayMode(IODisplayModeID *displayMode,
                                                        IOIndex *depth) {
    if (!displayMode || !depth) return kIOReturnBadArgument;
    *displayMode = _currentMode;
    *depth       = _currentDepth;
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEFramebuffer::setDisplayMode(IODisplayModeID displayMode,
                                                 IOIndex depth) {
    if (displayMode != kModeID_1080p) return kIOReturnBadArgument;
    _currentMode  = displayMode;
    _currentDepth = depth;
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEFramebuffer::getStartupDisplayMode(IODisplayModeID *displayMode,
                                                        IOIndex *depth) {
    if (!displayMode || !depth) return kIOReturnBadArgument;
    *displayMode = kModeID_1080p;
    *depth       = 0;
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEFramebuffer::getTimingInfoForDisplayMode(IODisplayModeID modeID,
                                                              IOTimingInformation *info) {
    if (!info || modeID != kModeID_1080p) return kIOReturnBadArgument;

    memset(info, 0, sizeof(*info));
    /* 1920x1080 @ 60Hz CEA-861 */
    info->appleTimingID = (IOAppleTimingID)0x57;
    info->detailedInfo.v2.horizontalActive          = kDisplayWidth;
    info->detailedInfo.v2.horizontalBlanking        = 280;
    info->detailedInfo.v2.horizontalSyncOffset      = 88;
    info->detailedInfo.v2.horizontalSyncPulseWidth  = 44;
    info->detailedInfo.v2.verticalActive            = kDisplayHeight;
    info->detailedInfo.v2.verticalBlanking          = 45;
    info->detailedInfo.v2.verticalSyncOffset        = 4;
    info->detailedInfo.v2.verticalSyncPulseWidth    = 5;
    info->detailedInfo.v2.pixelClock                = 148500;  /* kHz */
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEFramebuffer::getInformationForDisplayMode(IODisplayModeID modeID,
                                                               IODisplayModeInformation *info) {
    if (!info || modeID != kModeID_1080p) return kIOReturnBadArgument;

    memset(info, 0, sizeof(*info));
    info->maxDepthIndex    = 0;
    info->nominalWidth     = kDisplayWidth;
    info->nominalHeight    = kDisplayHeight;
    info->refreshRate      = 60 << 16; /* 60.0 Hz in 16.16 fixed point */
    info->flags            = kDisplayModeSafeFlag | kDisplayModeValidFlag;
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEFramebuffer::getPixelInformation(IODisplayModeID displayMode,
                                                      IOIndex depth,
                                                      IOPixelAperture aperture,
                                                      IOPixelInformation *pixelInfo) {
    if (!pixelInfo || displayMode != kModeID_1080p)
        return kIOReturnBadArgument;

    memset(pixelInfo, 0, sizeof(*pixelInfo));
    strncpy(pixelInfo->pixelFormat, IO32BitDirectPixels,
            sizeof(pixelInfo->pixelFormat) - 1);

    pixelInfo->activeWidth       = kDisplayWidth;
    pixelInfo->activeHeight      = kDisplayHeight;
    pixelInfo->bytesPerRow       = kStride;
    pixelInfo->bytesPerPlane     = (UInt32)_fbSize;
    pixelInfo->bitsPerPixel      = 32;
    pixelInfo->componentCount    = 3;
    pixelInfo->bitsPerComponent  = 8;
    pixelInfo->componentMasks[0] = 0x00FF0000; /* R */
    pixelInfo->componentMasks[1] = 0x0000FF00; /* G */
    pixelInfo->componentMasks[2] = 0x000000FF; /* B */
    pixelInfo->flags             = 0;
    /* apertureSample field removed from IOPixelInformation in Tahoe SDK */
    return kIOReturnSuccess;
}

UInt64 FakeIrisXEFramebuffer::getPixelFormatsForDisplayMode(IODisplayModeID modeID,
                                                              IOIndex depth) {
    /* Return bit 0 set = pixel format index 0 (IO32BitDirectPixels) is supported */
    return 0x1;
}

IOReturn FakeIrisXEFramebuffer::connectFlags(IOIndex connectIndex,
                                               IODisplayModeID displayMode,
                                               IOOptionBits *flags) {
    if (!flags) return kIOReturnBadArgument;
    *flags = kDisplayModeValidFlag | kDisplayModeSafeFlag;
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEFramebuffer::setCursorImage(void *cursorImage) {
    return kIOReturnUnsupported; /* software cursor via WindowServer */
}

IOReturn FakeIrisXEFramebuffer::setCursorState(SInt32 x, SInt32 y, bool visible) {
    return kIOReturnUnsupported;
}

/* ═══════════════════════════════════════════════════════════════════════
 * Stolen memory helpers (used by GEM for GGTT bootstrap)
 * ═══════════════════════════════════════════════════════════════════════ */

IOPhysicalAddress FakeIrisXEFramebuffer::getStolenMemBase() {
    uint32_t bgsm = _pciDevice->configRead32(0xC4); /* DSM base */
    return (IOPhysicalAddress)(bgsm & 0xFFF00000);  /* 1 MB aligned */
}

size_t FakeIrisXEFramebuffer::getStolenMemSize() {
    /* GGC register — bits [11:8] encode DSM size */
    uint32_t ggc = _pciDevice->configRead16(0x50);
    uint32_t sizeIdx = (ggc >> 8) & 0xF;
    static const size_t sizes[] = {
        0, 32, 64, 96, 128, 160, 192, 224,
        256, 288, 320, 352, 384, 448, 512, 1024
    };
    return sizes[sizeIdx] * 1024 * 1024;
}
