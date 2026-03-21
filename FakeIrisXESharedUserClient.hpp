/* FakeIrisXESharedUserClient.hpp */
#ifndef FAKEIRISXESHAREDUSERCLIENT_HPP
#define FAKEIRISXESHAREDUSERCLIENT_HPP
#include <IOKit/IOUserClient.h>
class FakeIrisXESharedUserClient : public IOUserClient {
    OSDeclareDefaultStructors(FakeIrisXESharedUserClient)
public:
    virtual bool initWithTask(task_t, void*, UInt32) override;
    virtual bool start(IOService*) override;
    virtual IOReturn clientClose() override;
    virtual IOReturn externalMethod(uint32_t, IOExternalMethodArguments*,
        IOExternalMethodDispatch*, OSObject*, void*) override;
};
#endif
