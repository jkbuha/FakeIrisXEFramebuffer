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
        /* Publish raw values as IORegistry properties — readable via ioreg */
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
            /* Disable PLL (0x46070 bit31), wait for lock clear (bit30) */
            uint32_t pll = mmioRead32(0x46070);
            mmioWrite32(0x46070, pll & ~(1u << 31));
            for (int t = 0; t < 500 && (mmioRead32(0x46070) & (1u<<30)); t++) IODelay(10);
            /* Set ratio=34 for 38.4MHz ref → 652.8MHz */
            pll = (mmioRead32(0x46070) & ~0xFF) | 34;
            mmioWrite32(0x46070, pll);
            /* Re-enable PLL, wait for lock */
            mmioWrite32(0x46070, pll | (1u << 31));
            for (int t = 0; t < 5000 && !(mmioRead32(0x46070) & (1u<<30)); t++) IODelay(10);
            /* Write new decimal freq to CDCLK_CTL bits[10:0] = 0x518 */
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
    /* TGL: enable PW1, PW2, then DDI-A/AUX-A */
    struct { uint32_t idx; const char *name; } pws[] = {
        { TGL_PW_CTL_IDX_PW_1,   "PW1"   },
        { TGL_PW_CTL_IDX_PW_2,   "PW2"   },
        { TGL_PW_CTL_IDX_DDI_A,  "DDI-A" },
        { TGL_PW_CTL_IDX_AUX_A,  "AUX-A" },
    };

    for (auto &pw : pws) {
        uint32_t ctlReg = HSW_PWR_WELL_CTL2;
        uint32_t reqBit = HSW_PWR_WELL_CTL_REQ(pw.idx);
        uint32_t staBit = HSW_PWR_WELL_CTL_STATE(pw.idx);

        uint32_t val = mmioRead32(ctlReg);
        mmioWrite32(ctlReg, val | reqBit);

        val = waitBits(ctlReg, staBit, staBit, 10);
        if (!(val & staBit)) {
            ERR("power well %s did not come up (CTL2=0x%08x)", pw.name, val);
            return kIOReturnTimeout;
        }
        LOG("power well %s enabled", pw.name);
    }
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

    /* Extended PW register dump */
    if (_mmioBase) {
        /* BIOS registers (read-only, show what firmware enabled) */
        setProperty("FXE-PW-BIOS1", (uint64_t)mmioRead32(0x45400), 32); /* CTL1 BIOS */
        setProperty("FXE-PW-BIOS2", (uint64_t)mmioRead32(0x45408), 32); /* CTL2 BIOS (pw=1) */
        setProperty("FXE-PW-BIOS3", (uint64_t)mmioRead32(0x45410), 32); /* CTL3 BIOS (pw=2) */
        /* Driver registers */
        setProperty("FXE-PW-DRV1",  (uint64_t)mmioRead32(0x45404), 32); /* CTL DRV pw=0 */
        setProperty("FXE-PW-DRV2",  (uint64_t)mmioRead32(0x4540C), 32); /* CTL DRV pw=1 */
        setProperty("FXE-PW-DRV3",  (uint64_t)mmioRead32(0x45414), 32); /* CTL DRV pw=2 */
        setProperty("FXE-PW-DRV14", (uint64_t)mmioRead32(0x45474), 32); /* CTL DRV pw=14 */
        setProperty("FXE-PW-DRV13", (uint64_t)mmioRead32(0x4546C), 32); /* CTL DRV pw=13 */
        /* FUSE_STATUS */
        setProperty("FXE-FUSE",     (uint64_t)mmioRead32(0x42000), 32);
        /* DC_STATE */
        setProperty("FXE-DC-STATE", (uint64_t)mmioRead32(0x45504), 32);
        /* Check if PW already on via BIOS regs */
        uint32_t bios1 = mmioRead32(0x45400);
        uint32_t bios2 = mmioRead32(0x45408);
        setProperty("FXE-PW1-BIOS-STA", (bios2 & 1) ? true : false);  /* state bit 0 of BIOS reg pw=1 */
        setProperty("FXE-PW2-BIOS-STA", (mmioRead32(0x45410) & 1) ? true : false);
    }
    /* Wide register scan to find PW13/14 state */
    if (_mmioBase) {
        for (uint32_t off = 0x45400; off <= 0x45430; off += 4) {
            uint32_t val = mmioRead32(off);
            if (val != 0) {
                char key[32];
                snprintf(key, sizeof(key), "FXE-REG-0x%05x", off);
                setProperty(key, (uint64_t)val, 32);
            }
        }
        /* Also check ICL_PWR_WELL_CTL (may be at different base on TGL) */
        setProperty("FXE-0x45410", (uint64_t)mmioRead32(0x45410), 32);
        setProperty("FXE-0x45418", (uint64_t)mmioRead32(0x45418), 32);
        setProperty("FXE-0x45420", (uint64_t)mmioRead32(0x45420), 32);
        setProperty("FXE-0x45428", (uint64_t)mmioRead32(0x45428), 32);
    }
    /* Read the actual per-well registers for PW1/PW2 */
    if (_mmioBase) {
        /* PW1: BIOS=0x45470, DRV=0x45474 */
        /* PW2: BIOS=0x45468, DRV=0x4546C */
        setProperty("FXE-PW1-BIOS-0x45470", (uint64_t)mmioRead32(0x45470), 32);
        setProperty("FXE-PW1-DRV-0x45474",  (uint64_t)mmioRead32(0x45474), 32);
        setProperty("FXE-PW2-BIOS-0x45468", (uint64_t)mmioRead32(0x45468), 32);
        setProperty("FXE-PW2-DRV-0x4546C",  (uint64_t)mmioRead32(0x4546C), 32);
        /* Also DDI-A and AUX-A */
        setProperty("FXE-DDIA-BIOS-0x45400", (uint64_t)mmioRead32(0x45400 + 0*8), 32);
        setProperty("FXE-DDIA-DRV-0x45404",  (uint64_t)mmioRead32(0x45404 + 0*8), 32);
    }
    /* Check DC_OFF and attempt correct TGL power well sequence */
    if (_mmioBase) {
        uint32_t dc_off_bios = mmioRead32(0x45400 + 9*8); /* 0x45448 */
        uint32_t dc_off_drv  = mmioRead32(0x45404 + 9*8); /* 0x4544C */
        setProperty("FXE-DCOFF-BIOS-0x45448", (uint64_t)dc_off_bios, 32);
        setProperty("FXE-DCOFF-DRV-0x4544C",  (uint64_t)dc_off_drv,  32);

        /* Try enabling DC_OFF first, then PW1 */
        /* Step 1: Request DC_OFF */
        uint32_t req = HSW_PWR_WELL_CTL_REQ(0);   /* bit 1 = 0x2 */
        uint32_t sta = HSW_PWR_WELL_CTL_STATE(0);  /* bit 0 = 0x1 */

        mmioWrite32(0x4544C, dc_off_drv | req);
        OSSynchronizeIO();
        for (int t = 0; t < 500; t++) {
            if (mmioRead32(0x4544C) & sta) break;
            IODelay(100);
        }
        uint32_t dc_off_after = mmioRead32(0x4544C);
        setProperty("FXE-DCOFF-AFTER", (uint64_t)dc_off_after, 32);
        bool dc_off_ok = (dc_off_after & sta) != 0;
        setProperty("FXE-DCOFF-OK", dc_off_ok);

        /* Step 2: Try PW1 after DC_OFF */
        if (dc_off_ok) {
            mmioWrite32(0x45474, req);
            OSSynchronizeIO();
            for (int t = 0; t < 500; t++) {
                if (mmioRead32(0x45474) & sta) break;
                IODelay(100);
            }
            uint32_t pw1_after = mmioRead32(0x45474);
            setProperty("FXE-PW1-AFTER-DCOFF", (uint64_t)pw1_after, 32);
            setProperty("FXE-PW1-OK-v2", (pw1_after & sta) != 0);
        }
    }
    /* Raw write/read test on PW1 register */
    if (_mmioBase) {
        /* Read PW1 DRV before */
        uint32_t pw1_before = mmioRead32(0x45474);
        setProperty("FXE-PW1-RAW-BEFORE", (uint64_t)pw1_before, 32);

        /* Write request bit */
        mmioWrite32(0x45474, 0x2);
        OSSynchronizeIO();
        uint32_t pw1_immed = mmioRead32(0x45474);
        setProperty("FXE-PW1-RAW-IMMED", (uint64_t)pw1_immed, 32);

        /* Try writing 0xFFFFFFFF to see what bits stick */
        mmioWrite32(0x45474, 0xFFFFFFFF);
        OSSynchronizeIO();
        uint32_t pw1_ff = mmioRead32(0x45474);
        setProperty("FXE-PW1-RAW-FF", (uint64_t)pw1_ff, 32);

        /* Restore */
        mmioWrite32(0x45474, pw1_before);

        /* Also try PW1 via the packed register approach
         * Some TGL docs show PWR_WELL_CTL as a single register
         * with all wells packed. Try 0x45400 directly with PW1 bits */
        uint32_t packed = mmioRead32(0x45400);
        setProperty("FXE-PACKED-0x45400", (uint64_t)packed, 32);
        /* Bit 29=PW1_REQ, 28=PW1_STA in packed layout */
        mmioWrite32(0x45400, packed | (1u<<29));
        OSSynchronizeIO();
        uint32_t packed_after = mmioRead32(0x45400);
        setProperty("FXE-PACKED-AFTER", (uint64_t)packed_after, 32);
        mmioWrite32(0x45400, packed); /* restore */
    }
    /* Implement pawan295 power well sequence exactly */
    if (_mmioBase && _pciDevice) {

        /* Step 1: Force PCI to D0 */
        _pciDevice->enablePCIPowerManagement(kPCIPMCSPowerStateD0);
        uint16_t pmcsr = _pciDevice->configRead16(0x84);
        pmcsr &= ~0x3;
        _pciDevice->configWrite16(0x84, pmcsr);
        IOSleep(10);
        setProperty("FXE-SEQ-PMCSR", (uint64_t)_pciDevice->configRead16(0x84), 16);

        /* Step 2: Disable GT power gating */
        uint32_t gt_pg = mmioRead32(0xA218);
        mmioWrite32(0xA218, gt_pg & ~0x1);
        IOSleep(10);
        setProperty("FXE-SEQ-GT-PG", (uint64_t)mmioRead32(0xA218), 32);

        /* Step 3: Disable PUNIT power gating */
        uint32_t punit = mmioRead32(0xA2B0);
        mmioWrite32(0xA2B0, punit & ~0x80000000);
        IOSleep(15);
        setProperty("FXE-SEQ-PUNIT", (uint64_t)mmioRead32(0xA2B0), 32);

        /* Step 4: Power Well 1 — write to BIOS CTL (0x45400), bits 1+2 */
        /* Status at 0x45408 bit 30 */
        uint32_t pw1_ctl = mmioRead32(0x45400);
        setProperty("FXE-SEQ-PW1-CTL-BEFORE", (uint64_t)pw1_ctl, 32);
        mmioWrite32(0x45400, pw1_ctl | 0x2);
        IOSleep(10);
        mmioWrite32(0x45400, mmioRead32(0x45400) | 0x4);
        IOSleep(10);

        bool pw1_up = false;
        for (int t = 0; t < 20; t++) {
            if (mmioRead32(0x45408) & (1u << 30)) { pw1_up = true; break; }
            IOSleep(10);
        }
        setProperty("FXE-SEQ-PW1-STATUS", (uint64_t)mmioRead32(0x45408), 32);
        setProperty("FXE-SEQ-PW1-OK", pw1_up);

        /* Step 5: Power Well 2 — write bit 0 to 0x45404, wait for 0xFF */
        uint32_t pw2_ctl = mmioRead32(0x45404);
        setProperty("FXE-SEQ-PW2-CTL-BEFORE", (uint64_t)pw2_ctl, 32);
        mmioWrite32(0x45404, pw2_ctl | 0x1);

        bool pw2_up = false;
        for (int t = 0; t < 50; t++) {
            if ((mmioRead32(0x45404) & 0xFF) == 0xFF) { pw2_up = true; break; }
            IOSleep(10);
        }
        setProperty("FXE-SEQ-PW2-STATUS", (uint64_t)mmioRead32(0x45404), 32);
        setProperty("FXE-SEQ-PW2-OK", pw2_up);

        /* Step 6: MBUS */
        mmioWrite32(0x7003C, 0xb1038c02);
        IOSleep(10);
        setProperty("FXE-SEQ-MBUS", (uint64_t)mmioRead32(0x7003C), 32);

        /* Step 7: LCPLL1 */
        mmioWrite32(0x46010, 0xcc000000);
        IOSleep(10);
        setProperty("FXE-SEQ-LCPLL1", (uint64_t)mmioRead32(0x46010), 32);

        /* Step 8: TRANS_CLK_SEL_A */
        mmioWrite32(0x46140, 0x10000000);
        IOSleep(10);
        setProperty("FXE-SEQ-TRANS-CLK", (uint64_t)mmioRead32(0x46140), 32);

            setProperty("FXE-SEQ-DONE", pw1_up && pw2_up);

        /* Display pipeline — only attempt if power wells are up */
        if (pw1_up && pw2_up) do {
            const uint32_t PIPE_SRC_A       = 0x6001C;
            const uint32_t PIPECONF_A       = 0x70008;
            const uint32_t PLANE_CTL_1_A    = 0x70180;
            const uint32_t PLANE_SURF_1_A   = 0x7019C;
            const uint32_t PLANE_STRIDE_1_A = 0x70188;
            const uint32_t PLANE_SIZE_1_A   = 0x70190;
            const uint32_t PLANE_POS_1_A    = 0x7018C;

            /* Step A: Allocate framebuffer (32MB) */
            const uint32_t fbSize = 32 * 1024 * 1024;
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

            /* Step B: Map GGTT (BAR1 = GTTMMADR at PCI config 0x18/0x1C) */
            {
                uint64_t bar1Lo = _pciDevice->configRead32(0x18) & ~0xFULL;
                uint64_t bar1Hi = _pciDevice->configRead32(0x1C);
                uint64_t gttPhys = (bar1Hi << 32) | bar1Lo;
                setProperty("FXE-GTT-PHYS", gttPhys, 64);
                LOG("hwInitAsync: GTTMMADR phys=0x%llx", gttPhys);

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
                const uint32_t fbGGTTOffset = 0x800;  /* aperture byte offset */
                const uint32_t ggttBaseIndex = fbGGTTOffset >> 12;  /* = 0 */
                const uint32_t kPageSize = 4096;

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
                setProperty("FXE-GTT-PAGES", (uint64_t)page, 32);
                LOG("hwInitAsync: %u GGTT pages installed at index %u", page, ggttBaseIndex);

                gttMap->release();
                gttDesc->release();
            }

            /* Step C: Fill framebuffer with solid colour (blue = 0x000000FF) for test */
            {
                uint32_t* pixels = (uint32_t*)fbMem->getBytesNoCopy();
                if (pixels) {
                    uint32_t count = (1920 * 1080);
                    for (uint32_t i = 0; i < count; i++) pixels[i] = 0x000000FF;
                }
            }

            /* Step D: Program plane geometry */
            uint32_t pipeSrc = mmioRead32(PIPE_SRC_A);
            setProperty("FXE-PIPE-SRC-BIOS", (uint64_t)pipeSrc, 32);
            if (!pipeSrc) mmioWrite32(PIPE_SRC_A, ((1920-1)<<16)|(1080-1));

            mmioWrite32(PLANE_POS_1_A,  0x00000000);
            mmioWrite32(PLANE_SIZE_1_A, ((1080-1)<<16)|(1920-1));
            mmioWrite32(PLANE_STRIDE_1_A, (1920*4) / 64);  /* 120 blocks */

            /* Watermarks */
            mmioWrite32(0xC4060, 0x00003FFF);
            mmioWrite32(0xC4064, 0x00000010);
            mmioWrite32(0xC4068, 0x00000020);
            mmioWrite32(0xC406C, 0x00000040);
            mmioWrite32(0xC4070, 0x00000080);
            mmioWrite32(0xC4020, 0x0000000F);

            /* Step E: Disable plane, write surface, re-enable */
            mmioWrite32(PLANE_CTL_1_A, mmioRead32(PLANE_CTL_1_A) & ~(1u<<31));
            (void)mmioRead32(PLANE_SURF_1_A);  /* flush */
            IOSleep(2);

            mmioWrite32(PLANE_SURF_1_A, 0x800);  /* GGTT offset */
            mmioWrite32(PLANE_STRIDE_1_A, (1920*4) / 64);

            uint32_t planeCtl = (1u<<31)|(0u<<24)|(1u<<3);  /* enable+XRGB8888+PipeA */
            mmioWrite32(PLANE_CTL_1_A, planeCtl);
            mmioWrite32(PLANE_SURF_1_A, 0x800);  /* re-write to trigger flip */
            (void)mmioRead32(PLANE_SURF_1_A);

            setProperty("FXE-PLANE-CTL-FINAL",  (uint64_t)mmioRead32(PLANE_CTL_1_A), 32);
            setProperty("FXE-PLANE-SURF-FINAL", (uint64_t)mmioRead32(PLANE_SURF_1_A), 32);

            /* PIPECONF */
            uint32_t pipeconf = mmioRead32(PIPECONF_A);
            if (!(pipeconf & (1u<<31))) {
                mmioWrite32(PIPECONF_A, pipeconf|(1u<<31)|(1u<<30));
                IOSleep(5);
            }
            setProperty("FXE-PIPECONF-FINAL", (uint64_t)mmioRead32(PIPECONF_A), 32);

            /* Panel power */
            mmioWrite32(0xC7200, mmioRead32(0xC7200) | 1);
            setProperty("FXE-PP-CTL-FINAL", (uint64_t)mmioRead32(0xC7200), 32);

            setProperty("FXE-DISPLAY-INIT", true);
            LOG("hwInitAsync: display pipeline complete — screen should show blue");

        } while (0); /* display pipeline block */
    }
    setProperty("FXE-Test-Done", true);
    LOG("requestProbe: HW test complete");
    return kIOReturnSuccess;
}


IOReturn FakeIrisXEFramebuffer::enableController() {
    static bool ran = false;
    if (ran) {
        LOG("enableController: already ran, skipping");
        return kIOReturnSuccess;
    }
    ran = true;
    LOG("enableController — running confirmed HW sequence");

    if (!_mmioBase || !_pciDevice) {
        ERR("enableController: no MMIO or PCI device");
        return kIOReturnError;
    }

    /* ── Step 1: Force PCI D0 ── */
    _pciDevice->enablePCIPowerManagement(kPCIPMCSPowerStateD0);
    _pciDevice->setBusMasterEnable(true);
    _pciDevice->setMemoryEnable(true);
    uint16_t pmcsr = _pciDevice->configRead16(0x84);
    _pciDevice->configWrite16(0x84, pmcsr & ~0x3);
    IODelay(10000);

    /* ── Step 2: FORCEWAKE ── */
    mmioWrite32(0xA278, 0x00010001);  /* FORCEWAKE_RENDER */
    for (int t = 0; t < 50; t++) {
        if (mmioRead32(0x0D84) & 1) break;
        IODelay(1000);
    }
    mmioWrite32(0xA188, 0x00010001);  /* FORCEWAKE_GT */
    for (int t = 0; t < 50; t++) {
        if (mmioRead32(0x130044) & 1) break;
        IODelay(1000);
    }
    LOG("enableController: FORCEWAKE done");

    /* ── Step 3: Disable GT + PUNIT power gating ── */
    mmioWrite32(0xA218, mmioRead32(0xA218) & ~0x1);
    IODelay(10000);
    mmioWrite32(0xA2B0, mmioRead32(0xA2B0) & ~0x80000000);
    IODelay(15000);

    /* ── Step 4: Power Well 1 (BIOS CTL 0x45400, bits 1+2) ── */
    mmioWrite32(0x45400, mmioRead32(0x45400) | 0x2);
    IODelay(10000);
    mmioWrite32(0x45400, mmioRead32(0x45400) | 0x4);
    IODelay(10000);
    bool pw1ok = false;
    for (int t = 0; t < 20; t++) {
        if (mmioRead32(0x45408) & (1u<<30)) { pw1ok = true; break; }
        IODelay(10000);
    }
    LOG("enableController: PW1 %s", pw1ok ? "UP" : "TIMEOUT");

    /* ── Step 5: Power Well 2 (DRV CTL 0x45404, bit 0) ── */
    mmioWrite32(0x45404, mmioRead32(0x45404) | 0x1);
    bool pw2ok = false;
    for (int t = 0; t < 50; t++) {
        if ((mmioRead32(0x45404) & 0xFF) == 0xFF) { pw2ok = true; break; }
        IODelay(10000);
    }
    LOG("enableController: PW2 %s", pw2ok ? "UP" : "TIMEOUT");

    /* ── Step 6: MBUS + display clocks ── */
    mmioWrite32(0x7003C, 0xb1038c02);
    IODelay(10000);
    mmioWrite32(0x46010, 0xcc000000);
    IODelay(10000);
    mmioWrite32(0x46140, 0x10000000);
    IODelay(10000);

    /* ── Step 7: Allocate framebuffer (32MB) ── */
    const uint32_t fbSize = 32 * 1024 * 1024;
    _fbMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task,
        kIODirectionInOut | kIOMemoryKernelUserShared,
        fbSize,
        0x000000003FFFF000ULL
    );
    if (!_fbMem || _fbMem->prepare() != kIOReturnSuccess) {
        ERR("enableController: FB alloc failed");
        if (_fbMem) { _fbMem->release(); _fbMem = nullptr; }
        return kIOReturnNoMemory;
    }
    LOG("enableController: FB phys=0x%llx", _fbMem->getPhysicalAddress());

    /* ── Step 8: Install GGTT page table entries ── */
    {
        uint64_t bar1Lo = _pciDevice->configRead32(0x18) & ~0xFULL;
        uint64_t bar1Hi = _pciDevice->configRead32(0x1C);
        uint64_t gttPhys = (bar1Hi << 32) | bar1Lo;

        IOMemoryDescriptor* gttDesc = IOMemoryDescriptor::withPhysicalAddress(
            gttPhys, 0x1000000, kIODirectionInOut);
        if (gttDesc) {
            IOMemoryMap* gttMap = gttDesc->map();
            if (gttMap) {
                volatile uint64_t* ggtt = (volatile uint64_t*)gttMap->getVirtualAddress();
                const uint32_t kPageSize = 4096;
                const uint32_t ggttBase  = 0x800 >> 12;  /* = 0 */
                IOByteCount offset = 0;
                uint32_t page = 0;
                while (offset < fbSize) {
                    IOByteCount segLen = 0;
                    IOPhysicalAddress segPhys = _fbMem->getPhysicalSegment(offset, &segLen);
                    if (!segPhys || !segLen) break;
                    segLen &= ~(kPageSize-1);
                    for (IOByteCount s = 0; s < segLen && offset < fbSize;
                         s += kPageSize, offset += kPageSize, ++page)
                        ggtt[ggttBase + page] = ((uint64_t)(segPhys+s) & ~0xFFFULL) | 0x3;
                }
                LOG("enableController: %u GGTT pages installed", page);
                gttMap->release();
            }
            gttDesc->release();
        }
    }

    /* ── Step 9: Fill with solid blue test pattern ── */
    {
        uint32_t* px = (uint32_t*)_fbMem->getBytesNoCopy();
        if (px) for (uint32_t i = 0; i < 1920*1080; i++) px[i] = 0x000000FF;
    }

    /* ── Step 10: Program display plane ── */
    /* Plane geometry */
    mmioWrite32(0x7018C, 0x00000000);            /* PLANE_POS_1_A  */
    mmioWrite32(0x70190, ((1080-1)<<16)|(1920-1)); /* PLANE_SIZE_1_A */
    mmioWrite32(0x70188, (1920*4)/64);             /* PLANE_STRIDE_1_A = 120 */

    /* Watermarks */
    mmioWrite32(0xC4060, 0x00003FFF);
    mmioWrite32(0xC4064, 0x00000010);
    mmioWrite32(0xC4068, 0x00000020);
    mmioWrite32(0xC406C, 0x00000040);
    mmioWrite32(0xC4070, 0x00000080);
    mmioWrite32(0xC4020, 0x0000000F);

    /* Disable plane, set surface, re-enable */
    mmioWrite32(0x70180, mmioRead32(0x70180) & ~(1u<<31)); /* PLANE_CTL disable */
    (void)mmioRead32(0x7019C);                              /* flush */
    IODelay(2000);
    mmioWrite32(0x7019C, 0x800);                            /* PLANE_SURF = GGTT offset */
    mmioWrite32(0x70188, (1920*4)/64);
    mmioWrite32(0x70180, (1u<<31)|(0u<<24)|(1u<<3));        /* enable+XRGB8888+PipeA */
    mmioWrite32(0x7019C, 0x800);                            /* re-write to flip */
    (void)mmioRead32(0x7019C);

    /* PIPECONF */
    uint32_t pipeconf = mmioRead32(0x70008);
    if (!(pipeconf & (1u<<31)))
        mmioWrite32(0x70008, pipeconf|(1u<<31)|(1u<<30));

    /* Panel power */
    mmioWrite32(0xC7200, mmioRead32(0xC7200) | 1);

    setProperty("FXE-EnableController-Done", true);
    setProperty("FXE-PW1-OK", pw1ok);
    setProperty("FXE-PW2-OK", pw2ok);
    LOG("enableController complete — display pipeline active");
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
    return true;
}

/* getApertureRange — pure virtual. Returns IODeviceMemory for the FB aperture. */
IODeviceMemory * FakeIrisXEFramebuffer::getApertureRange(IOPixelAperture aperture) {
    if (!_fbMem) return nullptr;
    IOPhysicalAddress phys = _fbMem->getPhysicalAddress();
    IOByteCount      len  = _fbMem->getLength();
    LOG("getApertureRange aperture=%d phys=0x%llx len=0x%llx", aperture, (uint64_t)phys, (uint64_t)len);
    return IODeviceMemory::withRange(phys, len);
}

/* getPixelFormats — pure virtual. Null-separated, double-null-terminated string list. */
const char * FakeIrisXEFramebuffer::getPixelFormats(void) {
    static const char fmt[] = "XRGB8888\0";
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
    /* timingCEA861_1920x1080p60 not in Tahoe SDK headers — use raw value 0x57 */
    info->appleTimingID = (IOAppleTimingID)0x57;
    /* Detailed timing */
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
