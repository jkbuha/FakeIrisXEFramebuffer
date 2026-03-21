/* FakeIrisXEGEM.cpp */

#include "FakeIrisXEGEM.hpp"
#include "FakeIrisXEFramebuffer.hpp"
#include <IOKit/IOLib.h>

OSDefineMetaClassAndStructors(FakeIrisXEGEM, OSObject)
#define KEXT_SUPER OSObject

#define LOG(fmt, ...) IOLog("FakeIrisXEGEM: " fmt "\n", ##__VA_ARGS__)
#define ERR(fmt, ...) IOLog("FakeIrisXEGEM ERROR: " fmt "\n", ##__VA_ARGS__)

/* GGTT PTE table lives in BAR0 at offset 0x800000 on TGL */
#define TGL_GGTT_PTE_OFFSET     0x800000
#define GGTT_START_OFFSET       0x1000   /* skip first page (reserved) */

bool FakeIrisXEGEM::init(FakeIrisXEFramebuffer *fb) {
    if (!KEXT_SUPER::init()) return false;
    _fb = fb;

    /* Map the GGTT PTE table from BAR0+0x800000 */
    IOMemoryMap *mmioMap = fb->getMMIOMap();
    if (!mmioMap) {
        ERR("no MMIO map from framebuffer");
        return false;
    }

    IOPhysicalAddress ptePhys = mmioMap->getPhysicalAddress() + TGL_GGTT_PTE_OFFSET;
    _ggttSize = 512 * 1024; /* 512 KB of PTEs = 64K entries × 8 bytes = 256 MB GGTT */

    /* We can't easily sub-map from an existing IOMemoryMap, so we use
     * the virtual address offset instead */
    volatile uint8_t *mmioBase = reinterpret_cast<volatile uint8_t *>(
        mmioMap->getVirtualAddress());
    _ggttPTEs = reinterpret_cast<volatile uint64_t *>(mmioBase + TGL_GGTT_PTE_OFFSET);

    _ggttCursor = GGTT_START_OFFSET;
    LOG("init — GGTT PTE table at virt+0x%x, managing 0x%zx bytes",
        TGL_GGTT_PTE_OFFSET, _ggttSize);
    return true;
}

void FakeIrisXEGEM::free() {
    KEXT_SUPER::free();
}

IOReturn FakeIrisXEGEM::createObject(size_t size, GEMObject *&outObj) {
    size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1); /* round up to page */

    GEMObject *obj = new GEMObject();
    if (!obj) return kIOReturnNoMemory;

    obj->mem = IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task,
        kIOMemoryPhysicallyContiguous | kIODirectionInOut,
        size, PAGE_SIZE);

    if (!obj->mem) { delete obj; return kIOReturnNoMemory; }
    obj->mem->prepare();
    obj->phys = obj->mem->getPhysicalAddress();
    obj->size = size;

    /* Assign GGTT address (bump allocator) */
    obj->ggttBase = _ggttCursor;
    _ggttCursor  += (uint32_t)size;

    /* Write PTEs */
    IOReturn ret = writeGGTTPTE(obj->ggttBase, obj->phys, size);
    if (ret != kIOReturnSuccess) {
        obj->mem->complete(); OSSafeReleaseNULL(obj->mem);
        delete obj; return ret;
    }

    obj->valid = true;
    outObj = obj;
    LOG("createObject: ggtt=0x%08x phys=0x%llx size=%zu",
        obj->ggttBase, (uint64_t)obj->phys, size);
    return kIOReturnSuccess;
}

void FakeIrisXEGEM::destroyObject(GEMObject *obj) {
    if (!obj) return;
    /* Zero PTEs */
    if (_ggttPTEs && obj->valid) {
        uint32_t start = obj->ggttBase / PAGE_SIZE;
        uint32_t pages = (uint32_t)(obj->size / PAGE_SIZE);
        for (uint32_t i = 0; i < pages; i++) _ggttPTEs[start + i] = 0;
        OSSynchronizeIO();
    }
    if (obj->mem) { obj->mem->complete(); OSSafeReleaseNULL(obj->mem); }
    obj->valid = false;
    delete obj;
}

IOReturn FakeIrisXEGEM::mapIntoGGTT(IOPhysicalAddress phys, size_t size,
                                       uint32_t ggttOffset) {
    return writeGGTTPTE(ggttOffset, phys, size);
}

IOReturn FakeIrisXEGEM::writeGGTTPTE(uint32_t ggttOffset, IOPhysicalAddress phys,
                                       size_t size) {
    if (!_ggttPTEs) return kIOReturnNotReady;

    uint32_t pageIdx  = ggttOffset / PAGE_SIZE;
    uint32_t numPages = (uint32_t)((size + PAGE_SIZE - 1) / PAGE_SIZE);

    for (uint32_t i = 0; i < numPages; i++) {
        uint64_t pte = ((uint64_t)phys + i * PAGE_SIZE) | GEN8_GGTT_PTE_VALID;
        _ggttPTEs[pageIdx + i] = pte;
    }
    OSSynchronizeIO();
    return kIOReturnSuccess;
}
