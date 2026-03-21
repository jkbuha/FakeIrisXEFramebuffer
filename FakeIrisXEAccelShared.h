/* FakeIrisXEAccelShared.h */
#ifndef FAKEIRISXEACCELSHARED_H
#define FAKEIRISXEACCELSHARED_H
#include <stdint.h>
enum FakeIrisXEAccelSelector {
    kAccelSelectGetDeviceInfo  = 0,
    kAccelSelectAllocBuffer    = 1,
    kAccelSelectFreeBuffer     = 2,
    kAccelSelectSubmitCommands = 3,
    kAccelSelectCount
};
typedef struct {
    uint32_t deviceID;
    uint32_t revisionID;
    uint32_t euCount;
    uint32_t vramMB;
    char     deviceName[64];
} AccelDeviceInfo;
#endif
