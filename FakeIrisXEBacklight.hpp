/* FakeIrisXEBacklight.hpp */
#ifndef FAKEIRISXEBACKLIGHT_HPP
#define FAKEIRISXEBACKLIGHT_HPP
#include <IOKit/IOService.h>
class FakeIrisXEBacklight : public IOService {
    OSDeclareDefaultStructors(FakeIrisXEBacklight)
public:
    virtual bool    start(IOService *provider) override;
    virtual void    stop(IOService *provider) override;
    IOReturn        setBrightness(uint32_t level);
};
#endif
