/* FakeIrisXEGuC.cpp
 * GuC firmware DMA loader + CTB ring setup.
 * Follows Linux i915 gen11+ GuC bring-up sequence.
 */

#include "FakeIrisXEGuC.hpp"
#include "FakeIrisXEFramebuffer.hpp"
#include "embedded_firmware.h"
#include "i915_reg.h"

#include <IOKit/IOLib.h>
#include <libkern/OSByteOrder.h>

OSDefineMetaClassAndStructors(FakeIrisXEGuC, OSObject)
#define KEXT_SUPER OSObject

#define LOG(fmt, ...) IOLog("FakeIrisXEGuC: " fmt "\n", ##__VA_ARGS__)
#define ERR(fmt, ...) IOLog("FakeIrisXEGuC ERROR: " fmt "\n", ##__VA_ARGS__)

/* -----------------------------------------------------------------------
 * WOPCM layout for TGL (from Intel GuC ABI)
 *   WOPCM size: 4 MB total
 *   GuC WOPCM offset: 0x15000 (84 KB) — must be > HuC fw size
 *   GuC WOPCM size:  up to 0x380000 (3.5 MB)
 * --------------------------------------------------------------------- */
#define TGL_WOPCM_OFFSET        GUC_WOPCM_OFFSET_VALUE   /* 0x15000 */
#define TGL_WOPCM_SIZE          0x380000
#define WOPCM_LOCK_BIT          REG_BIT(0)

bool FakeIrisXEGuC::init(FakeIrisXEFramebuffer *fb) {
    if (!KEXT_SUPER::init()) return false;
    _fb = fb;
    LOG("init");
    return true;
}

void FakeIrisXEGuC::free() {
    unload();
    KEXT_SUPER::free();
}

IOReturn FakeIrisXEGuC::load() {
#if !GUC_FIRMWARE_AVAILABLE
    LOG("GuC firmware not embedded — skipping (set GUC_FIRMWARE_AVAILABLE=1 and embed tgl_guc_70.bin)");
    return kIOReturnUnsupported;
#else
    IOReturn ret;

    LOG("load — starting GuC bring-up");

    ret = programWopcmLayout();
    if (ret != kIOReturnSuccess) { ERR("WOPCM layout failed"); return ret; }

    ret = dmaFirmwareToGuC(gTGLGuCFirmwareData, gTGLGuCFirmwareSize);
    if (ret != kIOReturnSuccess) { ERR("DMA failed"); return ret; }

    ret = waitGuCReady();
    if (ret != kIOReturnSuccess) { ERR("GuC never became ready"); return ret; }

    ret = setupCtbRings();
    if (ret != kIOReturnSuccess) { ERR("CTB ring setup failed"); return ret; }

    ret = registerCtbWithGuC();
    if (ret != kIOReturnSuccess) { ERR("CTB registration failed"); return ret; }

    _loaded = true;
    LOG("load — H2G CTB ready, GuC submission active");
    return kIOReturnSuccess;
#endif
}

void FakeIrisXEGuC::unload() {
    if (_ctbMem) {
        _ctbMem->complete();
        OSSafeReleaseNULL(_ctbMem);
    }
    _ctbPhys = 0;
    _ctbVirt = nullptr;
    _h2gDesc = nullptr;
    _g2hDesc = nullptr;
    _h2gRing = nullptr;
    _g2hRing = nullptr;
    _loaded  = false;
}

/* -----------------------------------------------------------------------
 * Step 1: Program WOPCM size and offset registers (must be before DMA)
 * --------------------------------------------------------------------- */
IOReturn FakeIrisXEGuC::programWopcmLayout() {
    /* Check if already locked (e.g. firmware set by BIOS) */
    uint32_t wopcmOffset = _fb->mmioRead32(GUC_WOPCM_OFFSET);
    if (wopcmOffset & WOPCM_LOCK_BIT) {
        LOG("WOPCM already locked by BIOS at offset=0x%08x", wopcmOffset);
        return kIOReturnSuccess;
    }

    LOG("programWopcmLayout — WOPCM: offset=0x%x size=0x%x",
        TGL_WOPCM_OFFSET, TGL_WOPCM_SIZE);

    _fb->mmioWrite32(GUC_WOPCM_SIZE,
                     (TGL_WOPCM_SIZE >> GUC_WOPCM_SIZE_SHIFT) | GUC_WOPCM_VALID);
    _fb->mmioWrite32(GUC_WOPCM_OFFSET,
                     (TGL_WOPCM_OFFSET >> GUC_WOPCM_GFXPAUSE_OFFSET_SHIFT) | GUC_WOPCM_VALID);
    OSSynchronizeIO();

    /* Lock */
    uint32_t val = _fb->mmioRead32(GUC_WOPCM_OFFSET);
    _fb->mmioWrite32(GUC_WOPCM_OFFSET, val | WOPCM_LOCK_BIT);
    return kIOReturnSuccess;
}

/* -----------------------------------------------------------------------
 * Step 2: DMA firmware blob to GuC WOPCM
 * Based on i915 intel_guc_fw.c:intel_guc_fw_upload()
 * --------------------------------------------------------------------- */
IOReturn FakeIrisXEGuC::dmaFirmwareToGuC(const uint8_t *fwData, size_t fwSize) {
    if (!fwData || fwSize == 0) return kIOReturnBadArgument;

    /* Allocate a DMA-capable buffer */
    IOBufferMemoryDescriptor *dmaMem = IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task,
        kIOMemoryPhysicallyContiguous | kIODirectionOut,
        fwSize, PAGE_SIZE);

    if (!dmaMem) return kIOReturnNoMemory;
    dmaMem->prepare();

    memcpy(dmaMem->getBytesNoCopy(), fwData, fwSize);
    IOPhysicalAddress dmaPhys = dmaMem->getPhysicalAddress();

    LOG("dmaFirmwareToGuC — firmware DMA: phys=0x%llx size=%zu",
        (uint64_t)dmaPhys, fwSize);

    /* Program DMA source (host physical address) */
    _fb->mmioWrite32(0xC340, (uint32_t)(dmaPhys & 0xFFFFFFFF));           /* DMA_ADDR_LO */
    _fb->mmioWrite32(0xC344, (uint32_t)((uint64_t)dmaPhys >> 32));        /* DMA_ADDR_HI */
    _fb->mmioWrite32(0xC348, (uint32_t)fwSize);                            /* DMA_COPY_SIZE */
    _fb->mmioWrite32(0xC34C, TGL_WOPCM_OFFSET);                           /* DMA_WOPCM_OFFSET */

    /* Trigger DMA transfer */
    _fb->mmioWrite32(DMA_CTRL, WOPCM_ENABLE | 0x01 /* start bit */);

    /* Wait for DMA complete (bit 0 clears) */
    uint32_t timeout = 1000;
    while ((_fb->mmioRead32(DMA_CTRL) & 0x01) && --timeout) {
        IODelay(100);
    }

    dmaMem->complete();
    OSSafeReleaseNULL(dmaMem);

    if (!timeout) {
        ERR("DMA timeout");
        return kIOReturnTimeout;
    }

    LOG("dmaFirmwareToGuC — firmware DMA complete (%zu bytes)", fwSize);
    return kIOReturnSuccess;
}

/* -----------------------------------------------------------------------
 * Step 3: Wait for GuC microkernel to report ready
 * GuC signals readiness by setting a status bit in scratch register
 * --------------------------------------------------------------------- */
#define GUC_SCRATCH_REG         _MMIO(0xC180)
#define GUC_READY_MASK          0xFF
#define GUC_READY_VALUE         0xA

IOReturn FakeIrisXEGuC::waitGuCReady() {
    LOG("waitGuCReady — polling GuC status");
    uint32_t timeout = 2000; /* 200 ms at 100µs intervals */
    while (--timeout) {
        uint32_t val = _fb->mmioRead32(GUC_SCRATCH_REG);
        if ((val & GUC_READY_MASK) == GUC_READY_VALUE) {
            LOG("GuC microkernel is running (scratch=0x%08x)", val);
            return kIOReturnSuccess;
        }
        IODelay(100);
    }
    ERR("GuC ready timeout (scratch=0x%08x)", _fb->mmioRead32(GUC_SCRATCH_REG));
    return kIOReturnTimeout;
}

/* -----------------------------------------------------------------------
 * Step 4: Setup H2G and G2H CTB rings in a contiguous allocation
 *
 *  Layout in _ctbMem (16 KB total):
 *  [0x0000] GucCtbDescriptor H2G descriptor
 *  [0x0020] GucCtbDescriptor G2H descriptor
 *  [0x1000] H2G ring data   (4 KB)
 *  [0x2000] G2H ring data   (4 KB)
 * --------------------------------------------------------------------- */
#define CTB_H2G_DESC_OFFSET  0x0000
#define CTB_G2H_DESC_OFFSET  0x0020
#define CTB_H2G_RING_OFFSET  0x1000
#define CTB_G2H_RING_OFFSET  0x2000
#define CTB_TOTAL_SIZE       0x3000

IOReturn FakeIrisXEGuC::setupCtbRings() {
    _ctbMem = IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task,
        kIOMemoryPhysicallyContiguous | kIODirectionInOut,
        CTB_TOTAL_SIZE, PAGE_SIZE);

    if (!_ctbMem) return kIOReturnNoMemory;
    _ctbMem->prepare();

    _ctbPhys = _ctbMem->getPhysicalAddress();
    _ctbVirt = reinterpret_cast<volatile uint8_t *>(_ctbMem->getBytesNoCopy());
    memset((void*)_ctbVirt, 0, CTB_TOTAL_SIZE);

    _h2gDesc = reinterpret_cast<GucCtbDescriptor *>((uint8_t*)_ctbVirt + CTB_H2G_DESC_OFFSET);
    _g2hDesc = reinterpret_cast<GucCtbDescriptor *>((uint8_t*)_ctbVirt + CTB_G2H_DESC_OFFSET);
    _h2gRing = _ctbVirt + CTB_H2G_RING_OFFSET;
    _g2hRing = _ctbVirt + CTB_G2H_RING_OFFSET;

    /* Fill H2G descriptor */
    _h2gDesc->offset = (uint32_t)(_ctbPhys + CTB_H2G_RING_OFFSET);
    _h2gDesc->size   = GUC_CTB_H2G_SIZE;
    _h2gDesc->head   = 0;
    _h2gDesc->tail   = 0;
    _h2gDesc->status = 0;

    /* Fill G2H descriptor */
    _g2hDesc->offset = (uint32_t)(_ctbPhys + CTB_G2H_RING_OFFSET);
    _g2hDesc->size   = GUC_CTB_G2H_SIZE;
    _g2hDesc->head   = 0;
    _g2hDesc->tail   = 0;
    _g2hDesc->status = 0;

    OSSynchronizeIO();
    LOG("setupCtbRings — H2G GGTT=0x%llx G2H GGTT=0x%llx desc GGTT=0x%llx",
        (uint64_t)(_ctbPhys + CTB_H2G_RING_OFFSET),
        (uint64_t)(_ctbPhys + CTB_G2H_RING_OFFSET),
        (uint64_t)_ctbPhys);
    return kIOReturnSuccess;
}

/* -----------------------------------------------------------------------
 * Step 5: Register CTB rings with GuC via MMIO
 * --------------------------------------------------------------------- */
IOReturn FakeIrisXEGuC::registerCtbWithGuC() {
    /* Write descriptor base and enable CTB */
    _fb->mmioWrite32(GUC_CT_BASE, (uint32_t)_ctbPhys);
    _fb->mmioWrite32(0xC304, (uint32_t)((uint64_t)_ctbPhys >> 32));  /* upper 32 bits */

    /* Enable CTB comms */
    uint32_t log = _fb->mmioRead32(GUC_LOG_CTL);
    _fb->mmioWrite32(GUC_LOG_CTL, log | GUC_LOG_VALID);

    OSSynchronizeIO();
    LOG("registerCtbWithGuC — CTB rings registered");
    return kIOReturnSuccess;
}

/* -----------------------------------------------------------------------
 * sendH2G — write an action message to the H2G CTB ring
 * --------------------------------------------------------------------- */
IOReturn FakeIrisXEGuC::sendH2G(uint32_t action, uint32_t *data, uint32_t dataLen) {
    if (!_loaded || !_h2gDesc || !_h2gRing) return kIOReturnNotReady;

    /* Compute message size: 1 header dword + dataLen dwords */
    uint32_t msgLen = (1 + dataLen) * sizeof(uint32_t);
    uint32_t tail   = _h2gDesc->tail;
    uint32_t head   = _h2gDesc->head;
    uint32_t avail  = (tail >= head)
                        ? (_h2gDesc->size - tail + head)
                        : (head - tail);

    if (avail < msgLen) {
        ERR("sendH2G: H2G ring full (avail=%u needed=%u)", avail, msgLen);
        return kIOReturnNoMemory;
    }

    /* Header: [31:16]=len [15:0]=action */
    uint32_t header = (msgLen << 16) | (action & 0xFFFF);
    uint32_t *ring  = (uint32_t *)(_h2gRing + tail);
    ring[0] = header;
    for (uint32_t i = 0; i < dataLen; i++) ring[1 + i] = data[i];

    OSSynchronizeIO();
    _h2gDesc->tail = (tail + msgLen) % _h2gDesc->size;
    OSSynchronizeIO();

    /* Ring the doorbell — write any value to GuC doorbell register */
    _fb->mmioWrite32(0xC004, 0x1);

    return kIOReturnSuccess;
}
