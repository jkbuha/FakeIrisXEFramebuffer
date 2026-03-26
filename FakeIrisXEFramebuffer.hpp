/* FakeIrisXEFramebuffer.hpp
 * IOFramebuffer subclass for Tiger Lake Iris Xe iGPU on macOS Sequoia
 *
 * Matches PCI device 0x9A498086 via IOPCIPrimaryMatch.
 * Provides: MMIO access, GT power, display pipeline (Pipe A / Trans A / Plane 1A),
 *           and acts as IOProvider for FakeIrisXEAccelerator.
 */

#ifndef FAKEIRISXEFRAMEBUFFER_HPP
#define FAKEIRISXEFRAMEBUFFER_HPP

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/graphics/IODisplay.h>

#include "i915_reg.h"

/* -----------------------------------------------------------------------
 * Forward declarations
 * --------------------------------------------------------------------- */
class FakeIrisXEGuC;
class FakeIrisXEExeclist;
class FakeIrisXEGEM;

/* -----------------------------------------------------------------------
 * Display timing for 1920×1080 @ 60 Hz (eDP)
 * --------------------------------------------------------------------- */
struct TGLDisplayTiming {
    uint32_t hActive;
    uint32_t hBlankStart;
    uint32_t hBlankEnd;
    uint32_t hSyncStart;
    uint32_t hSyncEnd;
    uint32_t hTotal;

    uint32_t vActive;
    uint32_t vBlankStart;
    uint32_t vBlankEnd;
    uint32_t vSyncStart;
    uint32_t vSyncEnd;
    uint32_t vTotal;

    uint32_t pixelClock_kHz;  /* 148500 for 1080p60 */
};

/* -----------------------------------------------------------------------
 * FakeIrisXEFramebuffer
 * --------------------------------------------------------------------- */
class FakeIrisXEFramebuffer : public IOFramebuffer {
    OSDeclareDefaultStructors(FakeIrisXEFramebuffer)

public:
    /* IOService lifecycle */
    virtual bool        init(OSDictionary *dict = nullptr) override;
    virtual bool        start(IOService *provider) override;
    virtual void        stop(IOService *provider) override;
    virtual void        free() override;

    /* IOFramebuffer API — signatures match Tahoe/Sequoia SDK IOFramebuffer.h
     * Key changes vs older SDKs:
     *   getPixelFormats()    -> const char* (void)
     *   getDisplayModeCount()-> IOItemCount (void)
     *   getApertureRange()   -> IODeviceMemory* [pure virtual, must implement]
     *   getPixelFormatsForDisplayMode() no longer exists
     */
    virtual IOReturn         enableController() override;
    virtual IOItemCount      getConnectionCount() override;

    /* Pure virtuals — all three must be implemented */
    virtual IODeviceMemory * getApertureRange(IOPixelAperture aperture) override;
    virtual const char *     getPixelFormats(void) override;
    virtual IOItemCount      getDisplayModeCount(void) override;

    virtual IOReturn    getAttributeForConnection(IOIndex connectIndex,
                                                   IOSelect attribute,
                                                   uintptr_t *value) override;
    virtual IOReturn    setAttributeForConnection(IOIndex connectIndex,
                                                   IOSelect attribute,
                                                   uintptr_t value) override;
    virtual bool        isConsoleDevice() override;
    virtual IOReturn    getTimingInfoForDisplayMode(IODisplayModeID modeID,
                                                     IOTimingInformation *info) override;
    virtual IOReturn    getDisplayModes(IODisplayModeID *allDisplayModes) override;
    virtual IOReturn    getCurrentDisplayMode(IODisplayModeID *displayMode,
                                               IOIndex *depth) override;
    virtual IOReturn    setDisplayMode(IODisplayModeID displayMode,
                                        IOIndex depth) override;
    virtual IOReturn    getInformationForDisplayMode(IODisplayModeID modeID,
                                                      IODisplayModeInformation *info) override;
    virtual IOReturn    getPixelInformation(IODisplayModeID displayMode,
                                             IOIndex depth,
                                             IOPixelAperture aperture,
                                             IOPixelInformation *pixelInfo) override;
    virtual IOReturn    getStartupDisplayMode(IODisplayModeID *displayMode,
                                               IOIndex *depth) override;
    virtual IOReturn    setCursorImage(void *cursorImage) override;
    virtual IOReturn    setCursorState(SInt32 x, SInt32 y, bool visible) override;
    /* getPixelFormatsForDisplayMode — pure virtual in Tahoe SDK IOFramebuffer.
     * Returns a bitmask of supported pixel format indices for the given mode/depth. */
    virtual UInt64      getPixelFormatsForDisplayMode(IODisplayModeID displayMode,
                                                       IOIndex depth) override;

    virtual IOReturn    connectFlags(IOIndex connectIndex,
                                     IODisplayModeID displayMode,
                                     IOOptionBits *flags) override;

    /* MMIO access — public so subsystems (GuC, Execlist) can use */
    uint32_t            mmioRead32(uint32_t offset);
    void                mmioWrite32(uint32_t offset, uint32_t value);
    void                mmioWrite64(uint32_t offset, uint64_t value);

    /* GT power */
    IOReturn            forcewakeGet();
    void                forcewakeRelease();

    /* Accessors for subsystems */
    IOPCIDevice *       getPCIDevice()  { return _pciDevice; }
    IOMemoryMap *       getMMIOMap()    { return _mmioMap; }
    FakeIrisXEGEM *     getGEM()        { return _gem; }

    /* User client — allows user-space tools to test hardware */
    virtual IOReturn    requestProbe(IOOptionBits options) override;

    /* GuC system init — called from start() */
    IOReturn            initGuCSystem();

    /* Stolen memory base (for GGTT) */
    IOPhysicalAddress   getStolenMemBase();
    size_t              getStolenMemSize();

private:
    /* Helpers */
    IOReturn            mapMMIO();
    IOReturn            initGT();
    IOReturn            enablePowerWells();
    IOReturn            initDisplayPipeline();
    IOReturn            programPipeA_1080p60();
    IOReturn            programPlane1A(IOPhysicalAddress fbPhys, uint32_t stride);
    IOReturn            allocateFramebuffer();

    uint32_t            waitBits(uint32_t offset, uint32_t mask,
                                 uint32_t expected, uint32_t timeoutMs);

    /* State */
    IOPCIDevice *       _pciDevice    = nullptr;
    IOMemoryMap *       _mmioMap      = nullptr;
    volatile uint8_t *  _mmioBase     = nullptr;
    size_t              _mmioSize     = 0;

    IOBufferMemoryDescriptor * _fbMemDesc  = nullptr;
    IOBufferMemoryDescriptor * _fbMem      = nullptr;
    IOPhysicalAddress          _fbPhysAddr = 0;
    void *                     _fbVirtAddr = nullptr;
    size_t                     _fbSize     = 0;

    FakeIrisXEGuC *     _guc      = nullptr;
    FakeIrisXEExeclist * _execlist = nullptr;
    FakeIrisXEGEM *     _gem      = nullptr;

    bool                _gtAwake     = false;
    bool                _displayInit = false;
    bool                _forcewakeHeld = false;

    IODisplayModeID     _currentMode  = 1;
    IOIndex             _currentDepth = 0;

    /* Display constants */
    static const uint32_t kDisplayWidth  = 1920;
    static const uint32_t kDisplayHeight = 1080;
    static const uint32_t kBytesPerPixel = 4;
    static const uint32_t kStride        = kDisplayWidth * kBytesPerPixel; /* 7680 */

    /* NDRV displacement state */
    volatile bool         _flipRunning   = false;
    uint32_t              _flipGGTTOff   = 0;
    uint32_t              _flipPlaneCtl  = 0;
};

#endif /* FAKEIRISXEFRAMEBUFFER_HPP */
