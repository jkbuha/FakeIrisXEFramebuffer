/* FakeIrisXEExeclist.cpp
 * LRC creation and Execlist submission engine.
 * Implements both Path A (GuC) and Path B (direct ELSP).
 */

#include "FakeIrisXEExeclist.hpp"
#include "FakeIrisXEFramebuffer.hpp"
#include "i915_reg.h"
#include <IOKit/IOLib.h>
#include <libkern/OSByteOrder.h>

OSDefineMetaClassAndStructors(FakeIrisXEExeclist, OSObject)
#define KEXT_SUPER OSObject

#define LOG(fmt, ...) IOLog("FakeIrisXEExeclist: " fmt "\n", ##__VA_ARGS__)
#define ERR(fmt, ...) IOLog("FakeIrisXEExeclist ERROR: " fmt "\n", ##__VA_ARGS__)

/* -----------------------------------------------------------------------
 * MI_NOOP command (32-bit, opcode 0)
 * MI_BATCH_BUFFER_END (opcode 0x0A << 23)
 * --------------------------------------------------------------------- */
#define MI_NOOP                 0x00000000
#define MI_BATCH_BUFFER_END     (0x0A << 23)
#define MI_ARB_ON_OFF           (0x01 << 23)
#define MI_ARB_ENABLE           REG_BIT(0)

/* -----------------------------------------------------------------------
 * init
 * --------------------------------------------------------------------- */
bool FakeIrisXEExeclist::init(FakeIrisXEFramebuffer *fb, bool gucMode) {
    if (!KEXT_SUPER::init()) return false;
    _fb      = fb;
    _gucMode = gucMode;

    LOG("init (mode=%s)", gucMode ? "GuC" : "direct-ELSP");

    /* Initialise RCS engine — the one we need for anything GPU-related */
    if (initEngine(kEngineRCS) != kIOReturnSuccess) {
        ERR("RCS engine init failed");
        return false;
    }

    /* Create a default context for RCS */
    IOReturn ret = createHwContextFor(kEngineRCS, _defaultCtx[kEngineRCS]);
    if (ret != kIOReturnSuccess) {
        ERR("failed to create default RCS context");
        return false;
    }

    LOG("init complete — RCS ready");
    return true;
}

/* -----------------------------------------------------------------------
 * Engine-level setup: enable Execlists mode in RING_MODE register
 * --------------------------------------------------------------------- */
IOReturn FakeIrisXEExeclist::initEngine(TGLEngineID engine) {
    uint32_t base = kEngineRingBase[engine];

    /* Enable execlist mode */
    uint32_t mode = _fb->mmioRead32(RING_MODE_GEN7(base));
    mode |= GFX_MODE_ENABLE_EXECLIST | (GFX_MODE_ENABLE_EXECLIST << 16);
    _fb->mmioWrite32(RING_MODE_GEN7(base), mode);

    LOG("initEngine %s: RING_MODE=0x%08x", kEngineNames[engine],
        _fb->mmioRead32(RING_MODE_GEN7(base)));
    return kIOReturnSuccess;
}

/* -----------------------------------------------------------------------
 * createHwContextFor — allocate and initialise a LRC + ring
 * --------------------------------------------------------------------- */
IOReturn FakeIrisXEExeclist::createHwContextFor(TGLEngineID engine,
                                                  TGLHwContext *&outCtx) {
    TGLHwContext *ctx = new TGLHwContext();
    if (!ctx) return kIOReturnNoMemory;

    /* Allocate LRC */
    ctx->lrcMem = IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task,
        kIOMemoryPhysicallyContiguous | kIODirectionInOut,
        LRC_SIZE, PAGE_SIZE);

    if (!ctx->lrcMem) { delete ctx; return kIOReturnNoMemory; }
    ctx->lrcMem->prepare();
    ctx->lrcPhys = ctx->lrcMem->getPhysicalAddress();
    ctx->lrcVirt = reinterpret_cast<volatile uint8_t *>(ctx->lrcMem->getBytesNoCopy());
    memset((void*)ctx->lrcVirt, 0, LRC_SIZE);

    /* Allocate command ring */
    ctx->ringMem = IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task,
        kIOMemoryPhysicallyContiguous | kIODirectionInOut,
        RING_SIZE, PAGE_SIZE);

    if (!ctx->ringMem) {
        ctx->lrcMem->complete(); OSSafeReleaseNULL(ctx->lrcMem);
        delete ctx; return kIOReturnNoMemory;
    }
    ctx->ringMem->prepare();
    ctx->ringPhys = ctx->ringMem->getPhysicalAddress();
    ctx->ringVirt = reinterpret_cast<volatile uint8_t *>(ctx->ringMem->getBytesNoCopy());
    memset((void*)ctx->ringVirt, 0, RING_SIZE);

    ctx->ctxId    = _nextCtxId++;
    ctx->ringTail = 0;
    ctx->valid    = true;

    /* Fill the LRC register state area */
    initLRC(ctx, engine);

    outCtx = ctx;
    LOG("createHwContextFor engine=%s ctxId=%u lrcGGTT=0x%llx ringGGTT=0x%llx",
        kEngineNames[engine], ctx->ctxId,
        (uint64_t)ctx->lrcPhys, (uint64_t)ctx->ringPhys);
    return kIOReturnSuccess;
}

/* -----------------------------------------------------------------------
 * initLRC — populate the LRC state page with register save/restore pairs
 * Format: alternating (register offset, value) dwords.
 * Starts at dword offset 1 (byte 4) of the LRC state page (second 4 KB).
 * --------------------------------------------------------------------- */
IOReturn FakeIrisXEExeclist::initLRC(TGLHwContext *ctx, TGLEngineID engine) {
    uint32_t base = kEngineRingBase[engine];

    /* The LRC state page begins at byte offset GEN11_LRC_PPHWSP_SIZE */
    volatile uint32_t *regs = reinterpret_cast<volatile uint32_t *>(
        (uint8_t*)ctx->lrcVirt + GEN11_LRC_PPHWSP_SIZE);

    /* Dword 0: context descriptor flags */
    regs[0] = 0; /* padding */

    /* Dword 1: CONTEXT_CONTROL register (with mask bits in hi 16 bits) */
    regs[CTX_CONTEXT_CONTROL * 2]     = RING_MI_MODE(base);  /* register offset */
    regs[CTX_CONTEXT_CONTROL * 2 + 1] =
        CONTEXT_CONTROL_RS_CTX_ENABLE |
        CONTEXT_CONTROL_ENGINE_CTX_RESTORE |
        (CONTEXT_CONTROL_RS_CTX_ENABLE << 16) |
        (CONTEXT_CONTROL_ENGINE_CTX_RESTORE << 16);

    /* RING_HEAD — 0 initially */
    regs[CTX_RING_HEAD * 2]     = RING_HEAD(base);
    regs[CTX_RING_HEAD * 2 + 1] = 0;

    /* RING_TAIL — 0 initially */
    regs[CTX_RING_TAIL * 2]     = RING_TAIL(base);
    regs[CTX_RING_TAIL * 2 + 1] = 0;

    /* RING_START — physical address of the ring */
    regs[CTX_RING_START * 2]     = RING_START(base);
    regs[CTX_RING_START * 2 + 1] = (uint32_t)(ctx->ringPhys & 0xFFFFF000);

    /* RING_CTL — size and VALID bit */
    regs[CTX_RING_CTL * 2]     = RING_CTL(base);
    regs[CTX_RING_CTL * 2 + 1] = RING_CTL_SIZE(RING_SIZE) | RING_VALID;

    /* PDP0 (page directory pointer) — identity/flat mapping for now */
    regs[CTX_PDP0_UDW * 2]     = 0x0230;  /* PDP0_UDW offset */
    regs[CTX_PDP0_UDW * 2 + 1] = 0;
    regs[CTX_PDP0_LDW * 2]     = 0x0234;  /* PDP0_LDW offset */
    regs[CTX_PDP0_LDW * 2 + 1] = 0;

    OSSynchronizeIO();
    return kIOReturnSuccess;
}

/* -----------------------------------------------------------------------
 * submitElsp — direct ELSP submission (Path B, no GuC)
 * Submit two contexts to ELSP register in correct order.
 * ELSP takes 2 × 64-bit descriptors, written as 4 × 32-bit words.
 * Upper 32 bits first, then lower 32 bits, for each slot.
 * --------------------------------------------------------------------- */
IOReturn FakeIrisXEExeclist::submitElsp(TGLEngineID engine,
                                         TGLHwContext *ctx0,
                                         TGLHwContext *ctx1) {
    if (!ctx0 || !ctx0->valid) return kIOReturnBadArgument;

    uint32_t base = kEngineRingBase[engine];

    /* Context descriptor format (Gen8+):
     *   [63:32] upper: LRC GGTT phys address >> 12 (page-aligned)
     *   [31:12] lower: context ID
     *   [11:0]  lower: flags (bit 0 = valid)
     */
    auto makeDesc = [](TGLHwContext *ctx) -> uint64_t {
        uint64_t upper = (uint64_t)(ctx->lrcPhys >> 12);
        uint64_t lower = ((uint64_t)ctx->ctxId << 12) | 0x01; /* valid */
        return (upper << 32) | lower;
    };

    uint64_t desc0 = makeDesc(ctx0);
    uint64_t desc1 = ctx1 ? makeDesc(ctx1) : 0;

    /* Write slot 1 first (lower priority), then slot 0 */
    _fb->mmioWrite32(RING_ELSP(base), (uint32_t)(desc1 >> 32));
    _fb->mmioWrite32(RING_ELSP(base), (uint32_t)(desc1 & 0xFFFFFFFF));
    _fb->mmioWrite32(RING_ELSP(base), (uint32_t)(desc0 >> 32));
    _fb->mmioWrite32(RING_ELSP(base), (uint32_t)(desc0 & 0xFFFFFFFF));
    OSSynchronizeIO();

    LOG("submitElsp engine=%s ctx0=%u desc0=0x%016llx",
        kEngineNames[engine], ctx0->ctxId, desc0);
    return kIOReturnSuccess;
}

/* -----------------------------------------------------------------------
 * submitNopBatch — emit a NOP batch buffer to RCS and submit it.
 * This is the "smoke test" — if this works, the pipeline is alive.
 * --------------------------------------------------------------------- */
IOReturn FakeIrisXEExeclist::submitNopBatch() {
    TGLHwContext *ctx = _defaultCtx[kEngineRCS];
    if (!ctx || !ctx->valid) return kIOReturnNotReady;

    /* Emit: 4× MI_NOOP, MI_BATCH_BUFFER_END, MI_NOOP (align to 8 bytes) */
    ringEmitNop(ctx, 4);
    ringEmitFlush(ctx);
    ringAdvanceTail(ctx);

    /* Update RING_TAIL in LRC state */
    volatile uint32_t *regs = reinterpret_cast<volatile uint32_t *>(
        (uint8_t*)ctx->lrcVirt + GEN11_LRC_PPHWSP_SIZE);
    regs[CTX_RING_TAIL * 2 + 1] = ctx->ringTail;
    OSSynchronizeIO();

    /* Submit via ELSP (direct mode) or GuC (GuC mode) */
    IOReturn ret;
    if (_gucMode) {
        /* GuC submission — send ACTION_SCHED_CONTEXT_MODE_SET via CTB */
        /* (Simplified — full GuC context submission requires more scaffolding) */
        LOG("submitNopBatch — GuC submission stub (not yet implemented)");
        ret = kIOReturnUnsupported;
    } else {
        ret = submitElsp(kEngineRCS, ctx, nullptr);
    }

    LOG("submitNopBatch: %s", ret == kIOReturnSuccess ? "submitted" : "failed");
    return ret;
}

/* -----------------------------------------------------------------------
 * Ring buffer helpers
 * --------------------------------------------------------------------- */
void FakeIrisXEExeclist::ringEmitNop(TGLHwContext *ctx, uint32_t count) {
    volatile uint32_t *ring = reinterpret_cast<volatile uint32_t *>(
        ctx->ringVirt + ctx->ringTail);
    for (uint32_t i = 0; i < count; i++) ring[i] = MI_NOOP;
    ctx->ringTail += count * sizeof(uint32_t);
}

void FakeIrisXEExeclist::ringEmitFlush(TGLHwContext *ctx) {
    volatile uint32_t *ring = reinterpret_cast<volatile uint32_t *>(
        ctx->ringVirt + ctx->ringTail);
    ring[0] = MI_BATCH_BUFFER_END;
    ring[1] = MI_NOOP; /* pad to 8-byte alignment */
    ctx->ringTail += 2 * sizeof(uint32_t);
}

void FakeIrisXEExeclist::ringAdvanceTail(TGLHwContext *ctx) {
    /* Tail must be 8-byte aligned */
    ctx->ringTail = (ctx->ringTail + 7) & ~7U;
    OSSynchronizeIO();
}

/* -----------------------------------------------------------------------
 * processCSB — read context status buffer for completed/idle events
 * --------------------------------------------------------------------- */
void FakeIrisXEExeclist::processCSB(TGLEngineID engine) {
    uint32_t base = kEngineRingBase[engine];
    uint32_t ptrReg = _fb->mmioRead32(RING_CONTEXT_STATUS_PTR(base));
    uint32_t head   = (ptrReg >> 8) & GEN8_CSB_PTR_MASK;
    uint32_t tail   = (ptrReg >> 0) & GEN8_CSB_PTR_MASK;

    while (head != tail) {
        uint32_t statusLo = _fb->mmioRead32(RING_CONTEXT_STATUS_BUF_LO(base, head));
        uint32_t statusHi = _fb->mmioRead32(RING_CONTEXT_STATUS_BUF_HI(base, head));

        if (statusLo & GEN8_CSB_STATUS_COMPLETE) {
            LOG("processCSB engine=%s head=%u — context COMPLETE (hi=0x%08x lo=0x%08x)",
                kEngineNames[engine], head, statusHi, statusLo);
        }
        if (statusLo & GEN8_CSB_STATUS_IDLE) {
            LOG("processCSB engine=%s — engine IDLE", kEngineNames[engine]);
        }

        head = (head + 1) & GEN8_CSB_PTR_MASK;
    }

    /* Write back updated head */
    _fb->mmioWrite32(RING_CONTEXT_STATUS_PTR(base),
                     (head << 8) | (tail & GEN8_CSB_PTR_MASK));
}

/* -----------------------------------------------------------------------
 * teardown / free
 * --------------------------------------------------------------------- */
void FakeIrisXEExeclist::teardown() {
    for (int i = 0; i < kEngineCount; i++) {
        if (_defaultCtx[i]) {
            if (_defaultCtx[i]->ringMem) {
                _defaultCtx[i]->ringMem->complete();
                OSSafeReleaseNULL(_defaultCtx[i]->ringMem);
            }
            if (_defaultCtx[i]->lrcMem) {
                _defaultCtx[i]->lrcMem->complete();
                OSSafeReleaseNULL(_defaultCtx[i]->lrcMem);
            }
            delete _defaultCtx[i];
            _defaultCtx[i] = nullptr;
        }
    }
}

void FakeIrisXEExeclist::free() {
    teardown();
    KEXT_SUPER::free();
}
