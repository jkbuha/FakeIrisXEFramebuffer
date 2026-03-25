/* FakeIrisXEFramebuffer.hpp
 * IOFramebuffer subclass for Intel Tiger Lake Iris Xe iGPU
 * Complete IOFramebuffer implementation (pawan architecture + our HW init)
 */

#ifndef FAKEIRISXEFRAMEBUFFER_HPP
#define FAKEIRISXEFRAMEBUFFER_HPP

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/graphics/IODisplay.h>

/* Forward declarations */
class FakeIrisXEGuC;
class FakeIrisXEExeclist;
class FakeIrisXEGEM;

/* Display mode info structure */
struct FXEDisplayModeInfo {
    IODisplayModeID   modeID;
    uint32_t          width;
    uint32_t          height;
    uint32_t          refreshRate;
    IOTimingInformation timing;
};

/* VBlank interrupt info */
struct FXEInterruptInfo {
    IOFBInterruptProc proc;
    OSObject         *target;
    void             *ref;
};

static const uint32_t kFXEMaxModes = 16;

class FakeIrisXEFramebuffer : public IOFramebuffer {
    OSDeclareDefaultStructors(FakeIrisXEFramebuffer)

public:
    /* IOService lifecycle */
    virtual bool        init(OSDictionary *dict = nullptr) override;
    virtual bool        start(IOService *provider) override;
    virtual void        stop(IOService *provider) override;
    virtual void        free() override;

    /* IOFramebuffer API — complete implementation */
    virtual IOReturn         enableController() override;
    virtual IOItemCount      getConnectionCount() override;
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
    virtual UInt64      getPixelFormatsForDisplayMode(IODisplayModeID displayMode,
                                                       IOIndex depth) override;
    virtual IOReturn    connectFlags(IOIndex connectIndex,
                                     IODisplayModeID displayMode,
                                     IOOptionBits *flags) override;

    /* Additional IOFramebuffer overrides — required for NDRV displacement */
    virtual IOReturn    getAttribute(IOSelect attribute, uintptr_t *value) override;
    virtual IOReturn    setAttribute(IOSelect attribute, uintptr_t value) override;
    virtual IOReturn    registerForInterruptType(IOSelect interruptType,
                                                  IOFBInterruptProc proc,
                                                  OSObject *target,
                                                  void *ref,
                                                  void **interruptRef) override;
    virtual IOReturn    unregisterInterrupt(void *interruptRef) override;
    virtual bool        hasDDCConnect(IOIndex connectIndex) override;
    virtual IOReturn    getDDCBlock(IOIndex connectIndex, UInt32 blockNumber,
                                    IOSelect blockType, IOOptionBits options,
                                    UInt8 *data, IOByteCount *length) override;

    /* Power management */
    virtual IOReturn    setPowerState(unsigned long powerStateOrdinal,
                                      IOService *whatDevice) override;
    virtual unsigned long maxCapabilityForDomainState(IOPMPowerFlags domainState) override;
    virtual unsigned long initialPowerStateForDomainState(IOPMPowerFlags domainState) override;
    virtual IOReturn    powerStateWillChangeTo(IOPMPowerFlags capabilities,
                                                unsigned long stateNumber,
                                                IOService *whatDevice) override;
    virtual IOReturn    powerStateDidChangeTo(IOPMPowerFlags capabilities,
                                               unsigned long stateNumber,
                                               IOService *whatDevice) override;

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

    /* VSync interrupt delivery */
    void                fireVSyncInterrupt();
    static void         vsyncTimerFired(OSObject *owner, IOTimerEventSource *sender);

    /* GUI transition (pawan's console→GUI unlock) */
    void                forceGUITransition();
    static void         forceGUITimerFired(OSObject *target, IOTimerEventSource *sender);

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

    /* WindowServer memory descriptors */
    IOBufferMemoryDescriptor * _cursorMem  = nullptr;
    IODeviceMemory *           _vramRange  = nullptr;

    /* VSync / interrupt support */
    OSArray *                  _interruptList  = nullptr;
    IOTimerEventSource *       _vsyncTimer     = nullptr;
    IOTimerEventSource *       _forceGUITimer  = nullptr;

    /* Subsystems */
    FakeIrisXEGuC *     _guc      = nullptr;
    FakeIrisXEExeclist * _execlist = nullptr;
    FakeIrisXEGEM *     _gem      = nullptr;

    bool                _gtAwake     = false;
    bool                _displayInit = false;
    bool                _forcewakeHeld = false;

    IODisplayModeID     _currentMode  = 1;
    IOIndex             _currentDepth = 0;
    IOLock *            _lock         = nullptr;

    /* Display constants */
    static const uint32_t kDisplayWidth  = 1920;
    static const uint32_t kDisplayHeight = 1080;
    static const uint32_t kBytesPerPixel = 4;
    static const uint32_t kStride        = kDisplayWidth * kBytesPerPixel;
};

#endif /* FAKEIRISXEFRAMEBUFFER_HPP */
