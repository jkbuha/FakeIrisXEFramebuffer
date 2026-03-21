/* FakeIrisXEAccelContext.hpp */
#ifndef FAKEIRISXEACCELCONTEXT_HPP
#define FAKEIRISXEACCELCONTEXT_HPP
#include <IOKit/IOUserClient.h>
class FakeIrisXEAccelContext : public IOUserClient {
    OSDeclareDefaultStructors(FakeIrisXEAccelContext)
public:
    virtual bool initWithTask(task_t, void*, UInt32) override;
    virtual bool start(IOService*) override;
    virtual IOReturn clientClose() override;
    virtual IOReturn externalMethod(uint32_t, IOExternalMethodArguments*,
        IOExternalMethodDispatch*, OSObject*, void*) override;
};
#endif
