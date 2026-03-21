/* FakeIrisXEGEM.hpp + FakeIrisXEGEM.cpp (combined for clarity)
 * Graphics Execution Manager — GGTT (Global GTT) object lifecycle.
 *
 * Manages allocation of GGTT address space and mapping of physical pages
 * into the aperture so GPU engines can access them.
 *
 * On Tiger Lake, GGTT entries are 8 bytes (Gen8 PTE format).
 */

#ifndef FAKEIRISXEGEM_HPP
#define FAKEIRISXEGEM_HPP

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include "i915_reg.h"

class FakeIrisXEFramebuffer;

/* -----------------------------------------------------------------------
 * A GEM buffer object
 * --------------------------------------------------------------------- */
struct GEMObject {
    IOBufferMemoryDescriptor *  mem      = nullptr;
    IOPhysicalAddress           phys     = 0;   /* physical (host) address */
    uint32_t                    ggttBase = 0;   /* GGTT offset (GPU address) */
    size_t                      size     = 0;
    bool                        valid    = false;
};

/* -----------------------------------------------------------------------
 * FakeIrisXEGEM
 * --------------------------------------------------------------------- */
class FakeIrisXEGEM : public OSObject {
    OSDeclareDefaultStructors(FakeIrisXEGEM)

public:
    bool        init(FakeIrisXEFramebuffer *fb);
    void        free() override;

    /* Allocate a buffer and map it into GGTT */
    IOReturn    createObject(size_t size, GEMObject *&outObj);

    /* Unmap and free a buffer */
    void        destroyObject(GEMObject *obj);

    /* Map a physical address range into GGTT at a specific offset */
    IOReturn    mapIntoGGTT(IOPhysicalAddress phys, size_t size, uint32_t ggttOffset);

private:
    IOReturn    writeGGTTPTE(uint32_t ggttOffset, IOPhysicalAddress phys, size_t size);

    FakeIrisXEFramebuffer * _fb          = nullptr;
    uint32_t                _ggttCursor  = 0;   /* simple bump allocator */

    /* GGTT aperture base — mapped from BAR2 (or BAR0 hi) */
    IOMemoryMap *           _ggttMap     = nullptr;
    volatile uint64_t *     _ggttPTEs    = nullptr;
    size_t                  _ggttSize    = 0;
};

#endif /* FAKEIRISXEGEM_HPP */
