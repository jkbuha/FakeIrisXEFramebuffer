/* stubs.cpp — stub implementations for all secondary classes
 * These compile cleanly and return kIOReturnUnsupported where needed.
 * Replace method bodies incrementally as features are implemented.
 */

/* ─────────────────────────────────────────────────────────────────────
 * FakeIrisXEAcceleratorUserClient
 * ───────────────────────────────────────────────────────────────────── */
#include "FakeIrisXEAcceleratorUserClient.hpp"
#include <IOKit/IOLib.h>

OSDefineMetaClassAndStructors(FakeIrisXEAcceleratorUserClient, IOUserClient)

bool FakeIrisXEAcceleratorUserClient::initWithTask(task_t owningTask, void *secID,
                                                    UInt32 type) {
    return IOUserClient::initWithTask(owningTask, secID, type);
}

bool FakeIrisXEAcceleratorUserClient::start(IOService *provider) {
    IOLog("FakeIrisXEAcceleratorUserClient: start\n");
    return IOService::start(provider);
}

IOReturn FakeIrisXEAcceleratorUserClient::clientClose() {
    terminate();
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEAcceleratorUserClient::externalMethod(
    uint32_t selector, IOExternalMethodArguments *args,
    IOExternalMethodDispatch *dispatch, OSObject *target, void *ref) {
    IOLog("FakeIrisXEAcceleratorUserClient: externalMethod selector=%u\n", selector);
    return kIOReturnUnsupported;
}

/* ─────────────────────────────────────────────────────────────────────
 * FakeIrisXEAccelDevice
 * ───────────────────────────────────────────────────────────────────── */
#include "FakeIrisXEAccelDevice.hpp"

OSDefineMetaClassAndStructors(FakeIrisXEAccelDevice, IOService)

bool FakeIrisXEAccelDevice::start(IOService *provider) {
    IOLog("FakeIrisXEAccelDevice: start\n");
    if (!IOService::start(provider)) return false;
    setProperty("IOGVACapabilities", "basic");
    return true;
}
void FakeIrisXEAccelDevice::stop(IOService *provider) { IOService::stop(provider); }

/* ─────────────────────────────────────────────────────────────────────
 * FakeIrisXEAccelContext
 * ───────────────────────────────────────────────────────────────────── */
#include "FakeIrisXEAccelContext.hpp"

OSDefineMetaClassAndStructors(FakeIrisXEAccelContext, IOUserClient)

bool FakeIrisXEAccelContext::initWithTask(task_t t, void *s, UInt32 type) {
    return IOUserClient::initWithTask(t, s, type);
}
bool FakeIrisXEAccelContext::start(IOService *p) { return IOService::start(p); }
IOReturn FakeIrisXEAccelContext::clientClose() { terminate(); return kIOReturnSuccess; }
IOReturn FakeIrisXEAccelContext::externalMethod(uint32_t sel,
    IOExternalMethodArguments *a, IOExternalMethodDispatch *d,
    OSObject *t, void *r) { return kIOReturnUnsupported; }

/* ─────────────────────────────────────────────────────────────────────
 * FakeIrisXESharedUserClient
 * ───────────────────────────────────────────────────────────────────── */
#include "FakeIrisXESharedUserClient.hpp"

OSDefineMetaClassAndStructors(FakeIrisXESharedUserClient, IOUserClient)

bool FakeIrisXESharedUserClient::initWithTask(task_t t, void *s, UInt32 type) {
    return IOUserClient::initWithTask(t, s, type);
}
bool FakeIrisXESharedUserClient::start(IOService *p) { return IOService::start(p); }
IOReturn FakeIrisXESharedUserClient::clientClose() { terminate(); return kIOReturnSuccess; }
IOReturn FakeIrisXESharedUserClient::externalMethod(uint32_t sel,
    IOExternalMethodArguments *a, IOExternalMethodDispatch *d,
    OSObject *t, void *r) { return kIOReturnUnsupported; }

/* ─────────────────────────────────────────────────────────────────────
 * FakeIrisXEBacklight
 * ───────────────────────────────────────────────────────────────────── */
#include "FakeIrisXEBacklight.hpp"

OSDefineMetaClassAndStructors(FakeIrisXEBacklight, IOService)

bool FakeIrisXEBacklight::start(IOService *provider) {
    IOLog("FakeIrisXEBacklight: start\n");
    if (!IOService::start(provider)) return false;
    /* TODO: PWM backlight control via PCH/DPCD */
    return true;
}
void FakeIrisXEBacklight::stop(IOService *provider) { IOService::stop(provider); }
IOReturn FakeIrisXEBacklight::setBrightness(uint32_t level) {
    return kIOReturnUnsupported; /* stub */
}
