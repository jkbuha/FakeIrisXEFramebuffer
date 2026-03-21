/* FakeIrisXEAccelerator.cpp */
#include "FakeIrisXEAccelerator.hpp"
#include "FakeIrisXEFramebuffer.hpp"
#include "FakeIrisXEAcceleratorUserClient.hpp"
#include <IOKit/IOLib.h>

OSDefineMetaClassAndStructors(FakeIrisXEAccelerator, IOAccelerator)
#define KEXT_SUPER IOAccelerator

#define LOG(fmt, ...) IOLog("FakeIrisXEAccelerator: " fmt "\n", ##__VA_ARGS__)

bool FakeIrisXEAccelerator::start(IOService *provider) {
    LOG("start");
    _fb = OSDynamicCast(FakeIrisXEFramebuffer, provider);
    if (!_fb) { LOG("provider is not FakeIrisXEFramebuffer"); return false; }
    if (!KEXT_SUPER::start(provider)) return false;

    /* Publish Metal properties */
    setProperty("MetalPluginClassName", "FakeIrisXEAccelerator");
    setProperty("MetalPlugin", kOSBooleanTrue);
    setProperty("IOGVAHEVCDecode", kOSBooleanTrue);
    setProperty("IOGVAGPUSupported", kOSBooleanTrue);

    LOG("start complete — IOAccelerator published");
    return true;
}

void FakeIrisXEAccelerator::stop(IOService *provider) {
    LOG("stop");
    KEXT_SUPER::stop(provider);
}

IOReturn FakeIrisXEAccelerator::newUserClient(task_t owningTask, void *securityID,
                                                UInt32 type, IOUserClient **handler) {
    LOG("newUserClient type=%u", (unsigned)type);
    FakeIrisXEAcceleratorUserClient *client = new FakeIrisXEAcceleratorUserClient();
    if (!client) return kIOReturnNoMemory;
    if (!client->initWithTask(owningTask, securityID, type)) {
        client->release();
        return kIOReturnError;
    }
    if (!client->attach(this) || !client->start(this)) {
        client->detach(this);
        client->release();
        return kIOReturnError;
    }
    *handler = client;
    return kIOReturnSuccess;
}
