/* FakeIrisXEUserClient.cpp
 * IOUserClient for FakeIrisXEFramebuffer
 */

#include "FakeIrisXEUserClient.hpp"
#include "i915_reg.h"
#include <IOKit/IOLib.h>

#define KEXT_SUPER IOUserClient
OSDefineMetaClassAndStructors(FakeIrisXEUserClient, IOUserClient)
#define LOG(fmt, ...) IOLog("FakeIrisXEUserClient: " fmt "\n", ##__VA_ARGS__)
#define ERR(fmt, ...) IOLog("FakeIrisXEUserClient ERROR: " fmt "\n", ##__VA_ARGS__)

bool FakeIrisXEUserClient::initWithTask(task_t owningTask, void *securityToken,
                                         UInt32 type, OSDictionary *properties) {
    if (!KEXT_SUPER::initWithTask(owningTask, securityToken, type, properties))
        return false;
    _task = owningTask;
    LOG("initWithTask");
    return true;
}

bool FakeIrisXEUserClient::start(IOService *provider) {
    _driver = OSDynamicCast(FakeIrisXEFramebuffer, provider);
    if (!_driver) {
        ERR("provider is not FakeIrisXEFramebuffer");
        return false;
    }
    if (!KEXT_SUPER::start(provider)) return false;
    LOG("start — user client ready");
    return true;
}

void FakeIrisXEUserClient::stop(IOService *provider) {
    LOG("stop");
    KEXT_SUPER::stop(provider);
}

IOReturn FakeIrisXEUserClient::clientClose() {
    LOG("clientClose");
    terminate();
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEUserClient::externalMethod(uint32_t selector,
                                               IOExternalMethodArguments *args,
                                               IOExternalMethodDispatch *dispatch,
                                               OSObject *target,
                                               void *reference) {
    switch (selector) {
        case kFXEMethodRunHWTest: return methodRunHWTest(args);
        case kFXEMethodReadReg:   return methodReadReg(args);
        case kFXEMethodWriteReg:  return methodWriteReg(args);
        default:
            ERR("unknown selector %u", selector);
            return kIOReturnBadArgument;
    }
}

/* ── kFXEMethodRunHWTest ─────────────────────────────────────────────────────
 * Runs the full hardware init sequence and returns results as scalars.
 * No display pipeline — just FORCEWAKE, power wells, register checks.
 */
IOReturn FakeIrisXEUserClient::methodRunHWTest(IOExternalMethodArguments *args) {
    if (args->scalarOutputCount < kFXEOut_COUNT) {
        ERR("not enough output scalars (%u < %u)",
            args->scalarOutputCount, kFXEOut_COUNT);
        return kIOReturnBadArgument;
    }

    LOG("RunHWTest starting...");

    /* Zero outputs */
    for (uint32_t i = 0; i < kFXEOut_COUNT; i++)
        args->scalarOutput[i] = 0;

    /* ── Step 1: Read CD clock before anything ───────────────────────── */
    uint32_t cdclk_before = _driver->mmioRead32(0x46000) & 0x7FF;
    uint32_t dssm         = _driver->mmioRead32(0x51004);
    args->scalarOutput[kFXEOut_CDCLK_Before] = cdclk_before;
    args->scalarOutput[kFXEOut_DSSM]         = dssm;
    LOG("CDCLK before=0x%03x DSSM=0x%08x", cdclk_before, dssm);

    /* ── Step 2: FORCEWAKE ───────────────────────────────────────────── */
    IOReturn ret = _driver->forcewakeGet();
    if (ret != kIOReturnSuccess) {
        ERR("FORCEWAKE failed (0x%08x)", ret);
        args->scalarOutput[kFXEOut_ForcewakeOK] = 0;
        args->scalarOutput[kFXEOut_GTReady]     = 0;
        /* Still read CD clock after */
        args->scalarOutput[kFXEOut_CDCLK_After] = _driver->mmioRead32(0x46000) & 0x7FF;
        return kIOReturnSuccess; /* Return success so tool gets partial results */
    }
    args->scalarOutput[kFXEOut_ForcewakeOK] = 1;
    args->scalarOutput[kFXEOut_GTReady]     = 1;
    LOG("FORCEWAKE OK");

    /* ── Step 3: Power Well 1 ────────────────────────────────────────── */
    {
        uint32_t ctlReg = HSW_PWR_WELL_CTL2;
        uint32_t reqBit = HSW_PWR_WELL_CTL_REQ(TGL_PW_CTL_IDX_PW_1);
        uint32_t staBit = HSW_PWR_WELL_CTL_STATE(TGL_PW_CTL_IDX_PW_1);
        uint32_t val    = _driver->mmioRead32(ctlReg);
        _driver->mmioWrite32(ctlReg, val | reqBit);
        /* Poll for state bit — 50ms timeout */
        for (int t = 0; t < 500; t++) {
            if (_driver->mmioRead32(ctlReg) & staBit) break;
            IODelay(100);
        }
        uint32_t result = _driver->mmioRead32(ctlReg);
        args->scalarOutput[kFXEOut_PW1OK] = (result & staBit) ? 1 : 0;
        LOG("PW1 CTL2=0x%08x state=%s", result,
            (result & staBit) ? "ON" : "TIMEOUT");
    }

    /* ── Step 4: Power Well 2 ────────────────────────────────────────── */
    {
        uint32_t ctlReg = HSW_PWR_WELL_CTL2;
        uint32_t reqBit = HSW_PWR_WELL_CTL_REQ(TGL_PW_CTL_IDX_PW_2);
        uint32_t staBit = HSW_PWR_WELL_CTL_STATE(TGL_PW_CTL_IDX_PW_2);
        uint32_t val    = _driver->mmioRead32(ctlReg);
        _driver->mmioWrite32(ctlReg, val | reqBit);
        for (int t = 0; t < 500; t++) {
            if (_driver->mmioRead32(ctlReg) & staBit) break;
            IODelay(100);
        }
        uint32_t result = _driver->mmioRead32(ctlReg);
        args->scalarOutput[kFXEOut_PW2OK] = (result & staBit) ? 1 : 0;
        LOG("PW2 CTL2=0x%08x state=%s", result,
            (result & staBit) ? "ON" : "TIMEOUT");
    }

    /* ── Step 5: Read CD clock after power wells ─────────────────────── */
    args->scalarOutput[kFXEOut_CDCLK_After] = _driver->mmioRead32(0x46000) & 0x7FF;
    LOG("CDCLK after=0x%03x", (uint32_t)args->scalarOutput[kFXEOut_CDCLK_After]);

    _driver->forcewakeRelease();

    LOG("RunHWTest complete: FW=%llu PW1=%llu PW2=%llu CDCLK_before=0x%llx CDCLK_after=0x%llx",
        args->scalarOutput[kFXEOut_ForcewakeOK],
        args->scalarOutput[kFXEOut_PW1OK],
        args->scalarOutput[kFXEOut_PW2OK],
        args->scalarOutput[kFXEOut_CDCLK_Before],
        args->scalarOutput[kFXEOut_CDCLK_After]);

    return kIOReturnSuccess;
}

/* ── kFXEMethodReadReg ───────────────────────────────────────────────────────
 * Input:  scalar[0] = MMIO offset
 * Output: scalar[0] = register value
 */
IOReturn FakeIrisXEUserClient::methodReadReg(IOExternalMethodArguments *args) {
    if (args->scalarInputCount < 1 || args->scalarOutputCount < 1)
        return kIOReturnBadArgument;
    uint32_t offset = (uint32_t)args->scalarInput[0];
    args->scalarOutput[0] = _driver->mmioRead32(offset);
    LOG("ReadReg 0x%05x = 0x%08x", offset, (uint32_t)args->scalarOutput[0]);
    return kIOReturnSuccess;
}

/* ── kFXEMethodWriteReg ──────────────────────────────────────────────────────
 * Input:  scalar[0] = MMIO offset, scalar[1] = value
 */
IOReturn FakeIrisXEUserClient::methodWriteReg(IOExternalMethodArguments *args) {
    if (args->scalarInputCount < 2)
        return kIOReturnBadArgument;
    uint32_t offset = (uint32_t)args->scalarInput[0];
    uint32_t value  = (uint32_t)args->scalarInput[1];
    _driver->mmioWrite32(offset, value);
    LOG("WriteReg 0x%05x = 0x%08x", offset, value);
    return kIOReturnSuccess;
}
