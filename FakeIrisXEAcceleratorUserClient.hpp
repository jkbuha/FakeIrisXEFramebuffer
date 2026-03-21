/* FakeIrisXEAcceleratorUserClient.hpp */
#ifndef FAKEIRISXEACCELERATORUSERCLIENT_HPP
#define FAKEIRISXEACCELERATORUSERCLIENT_HPP
#include <IOKit/IOUserClient.h>
class FakeIrisXEAcceleratorUserClient : public IOUserClient {
    OSDeclareDefaultStructors(FakeIrisXEAcceleratorUserClient)
public:
    virtual bool    initWithTask(task_t owningTask, void *securityID, UInt32 type) override;
    virtual bool    start(IOService *provider) override;
    virtual IOReturn clientClose() override;
    virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                                     IOExternalMethodDispatch *dispatch,
                                     OSObject *target, void *reference) override;
};
#endif
