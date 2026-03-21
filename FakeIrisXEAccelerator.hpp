/* FakeIrisXEAccelerator.hpp */
#ifndef FAKEIRISXEACCELERATOR_HPP
#define FAKEIRISXEACCELERATOR_HPP
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/graphics/IOAccelerator.h>

class FakeIrisXEFramebuffer;

class FakeIrisXEAccelerator : public IOAccelerator {
    OSDeclareDefaultStructors(FakeIrisXEAccelerator)
public:
    virtual bool    start(IOService *provider) override;
    virtual void    stop(IOService *provider) override;
    virtual IOReturn newUserClient(task_t owningTask, void *securityID,
                                    UInt32 type, IOUserClient **handler) override;
private:
    FakeIrisXEFramebuffer * _fb = nullptr;
};
#endif
