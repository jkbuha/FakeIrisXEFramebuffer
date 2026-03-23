/* FakeIrisXEUserClient.hpp
 * IOUserClient for FakeIrisXEFramebuffer — exposes hardware test methods
 * to user space without touching the IOFramebuffer display category.
 *
 * Usage from user space:
 *   io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault,
 *       IOServiceMatching("FakeIrisXEFramebuffer"));
 *   io_connect_t conn;
 *   IOServiceOpen(svc, mach_task_self(), 0, &conn);
 *   IOConnectCallScalarMethod(conn, kFXEMethodRunHWTest, NULL, 0, out, &outCnt);
 */

#ifndef FAKEIRISXEUSERCLIENT_HPP
#define FAKEIRISXEUSERCLIENT_HPP

#include <IOKit/IOUserClient.h>
#include "FakeIrisXEFramebuffer.hpp"

/* Method selectors — must match tool */
enum FXEUserClientMethod {
    kFXEMethodRunHWTest  = 0,   /* Run FORCEWAKE + power wells + register dump */
    kFXEMethodReadReg    = 1,   /* Read single MMIO register */
    kFXEMethodWriteReg   = 2,   /* Write single MMIO register */
    kFXEMethodCount
};

/* Output structure for kFXEMethodRunHWTest (packed into scalar outputs) */
/* Indices into the output scalar array */
enum FXEHWTestOutput {
    kFXEOut_ForcewakeOK  = 0,   /* 1 = FORCEWAKE ACK received */
    kFXEOut_PW1OK        = 1,   /* 1 = Power Well 1 enabled */
    kFXEOut_PW2OK        = 2,   /* 1 = Power Well 2 enabled */
    kFXEOut_CDCLK_Before = 3,   /* CD clock freq field before reprogram */
    kFXEOut_CDCLK_After  = 4,   /* CD clock freq field after reprogram */
    kFXEOut_DSSM         = 5,   /* DSSM register value */
    kFXEOut_GTReady      = 6,   /* 1 = GT responded to FORCEWAKE */
    kFXEOut_COUNT        = 7
};

class FakeIrisXEUserClient : public IOUserClient {
    OSDeclareDefaultStructors(FakeIrisXEUserClient)

public:
    virtual bool        initWithTask(task_t owningTask, void *securityToken,
                                     UInt32 type, OSDictionary *properties) override;
    virtual bool        start(IOService *provider) override;
    virtual void        stop(IOService *provider) override;
    virtual IOReturn    clientClose() override;
    virtual IOReturn    externalMethod(uint32_t selector,
                                       IOExternalMethodArguments *arguments,
                                       IOExternalMethodDispatch *dispatch,
                                       OSObject *target,
                                       void *reference) override;

private:
    IOReturn            methodRunHWTest(IOExternalMethodArguments *args);
    IOReturn            methodReadReg(IOExternalMethodArguments *args);
    IOReturn            methodWriteReg(IOExternalMethodArguments *args);

    FakeIrisXEFramebuffer * _driver = nullptr;
    task_t                  _task   = nullptr;
};

#endif /* FAKEIRISXEUSERCLIENT_HPP */
