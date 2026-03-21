/* FakeIrisXEGuC.hpp
 * GuC microkernel firmware loader and Command Transport Buffer (CTB) manager.
 *
 * When GUC_FIRMWARE_AVAILABLE == 0 in embedded_firmware.h, load() returns
 * kIOReturnUnsupported and the caller falls back to legacy Execlists.
 */

#ifndef FAKEIRISXEGUC_HPP
#define FAKEIRISXEGUC_HPP

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include "i915_reg.h"

class FakeIrisXEFramebuffer;

/* CTB ring descriptor (shared with GuC) */
struct GucCtbDescriptor {
    uint32_t offset;   /* GGTT offset of the ring */
    uint32_t size;     /* ring size in bytes */
    uint32_t head;
    uint32_t tail;
    uint32_t status;
    uint32_t rsvd[3];
};

/* CTB ring layout — H2G (host-to-GuC) and G2H (GuC-to-host) */
#define GUC_CTB_H2G_SIZE    (4 * 1024)
#define GUC_CTB_G2H_SIZE    (4 * 1024)
#define GUC_CTB_DESC_SIZE   (2 * sizeof(GucCtbDescriptor))

class FakeIrisXEGuC : public OSObject {
    OSDeclareDefaultStructors(FakeIrisXEGuC)

public:
    bool            init(FakeIrisXEFramebuffer *fb);
    IOReturn        load();      /* DMA firmware, wait for GuC ready, setup CTB */
    void            unload();
    void            free() override;

    bool            isLoaded() const { return _loaded; }

    /* Send a message via H2G CTB */
    IOReturn        sendH2G(uint32_t action, uint32_t *data, uint32_t dataLen);

private:
    IOReturn        programWopcmLayout();
    IOReturn        dmaFirmwareToGuC(const uint8_t *fwData, size_t fwSize);
    IOReturn        waitGuCReady();
    IOReturn        setupCtbRings();
    IOReturn        registerCtbWithGuC();

    FakeIrisXEFramebuffer *     _fb      = nullptr;

    /* CTB descriptor + ring backing memory */
    IOBufferMemoryDescriptor *  _ctbMem  = nullptr;
    IOPhysicalAddress           _ctbPhys = 0;
    volatile uint8_t *          _ctbVirt = nullptr;

    GucCtbDescriptor *  _h2gDesc = nullptr;
    GucCtbDescriptor *  _g2hDesc = nullptr;
    volatile uint8_t *  _h2gRing = nullptr;
    volatile uint8_t *  _g2hRing = nullptr;

    bool    _loaded = false;
};

#endif /* FAKEIRISXEGUC_HPP */
