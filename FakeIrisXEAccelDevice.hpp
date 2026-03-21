/* FakeIrisXEAccelDevice.hpp */
#ifndef FAKEIRISXEACCELDEVICE_HPP
#define FAKEIRISXEACCELDEVICE_HPP
#include <IOKit/IOService.h>
class FakeIrisXEAccelDevice : public IOService {
    OSDeclareDefaultStructors(FakeIrisXEAccelDevice)
public:
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;
};
#endif
