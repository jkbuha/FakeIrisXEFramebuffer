/* FakeIrisXEExeclist.hpp
 * Logical Ring Context (LRC) manager and Execlist command submission.
 *
 * Supports both:
 *   Path A: GuC-mediated submission (when gucMode == true)
 *   Path B: Direct ELSP submission  (legacy, no GuC)
 *
 * Each engine has:
 *   - A Logical Ring Context (LRC) — 8 KB per context (PPHWSP + state)
 *   - A command ring buffer    — separate allocation
 *   - A hardware context ID    — assigned on creation
 */

#ifndef FAKEIRISXEEXECLIST_HPP
#define FAKEIRISXEEXECLIST_HPP

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include "i915_reg.h"

class FakeIrisXEFramebuffer;

/* -----------------------------------------------------------------------
 * Hardware engine IDs for Tiger Lake
 * --------------------------------------------------------------------- */
enum TGLEngineID {
    kEngineRCS   = 0,  /* Render / 3D */
    kEngineBCS   = 1,  /* Blitter (copy) */
    kEngineVCS0  = 2,  /* Video codec 0 */
    kEngineVECS0 = 3,  /* Video enhancement 0 */
    kEngineCount = 4
};

/* -----------------------------------------------------------------------
 * Ring base addresses for each engine
 * --------------------------------------------------------------------- */
static const uint32_t kEngineRingBase[kEngineCount] = {
    RENDER_RING_BASE,   /* RCS */
    BLT_RING_BASE,      /* BCS */
    GEN6_BSD_RING_BASE, /* VCS0 */
    VEBOX_RING_BASE     /* VECS0 */
};

static const char * const kEngineNames[kEngineCount] = {
    "RCS", "BCS", "VCS0", "VECS0"
};

/* -----------------------------------------------------------------------
 * Per-context state
 * --------------------------------------------------------------------- */
#define RING_SIZE           (256 * 1024)   /* 256 KB command ring */
#define LRC_SIZE            GEN11_LRC_SIZE /* 8 KB */

struct TGLHwContext {
    /* LRC backing memory */
    IOBufferMemoryDescriptor *  lrcMem    = nullptr;
    IOPhysicalAddress           lrcPhys   = 0;
    volatile uint8_t *          lrcVirt   = nullptr;

    /* Command ring */
    IOBufferMemoryDescriptor *  ringMem   = nullptr;
    IOPhysicalAddress           ringPhys  = 0;
    volatile uint8_t *          ringVirt  = nullptr;
    uint32_t                    ringTail  = 0;  /* submission tail */

    uint32_t    ctxId     = 0;
    bool        valid     = false;
};

/* -----------------------------------------------------------------------
 * FakeIrisXEExeclist
 * --------------------------------------------------------------------- */
class FakeIrisXEExeclist : public OSObject {
    OSDeclareDefaultStructors(FakeIrisXEExeclist)

public:
    bool        init(FakeIrisXEFramebuffer *fb, bool gucMode);
    void        teardown();
    void        free() override;

    /* Create a hardware context for an engine */
    IOReturn    createHwContextFor(TGLEngineID engine, TGLHwContext *&outCtx);

    /* Submit a context pair to ELSP (direct mode, Path B) */
    IOReturn    submitElsp(TGLEngineID engine,
                           TGLHwContext *ctx0, TGLHwContext *ctx1);

    /* Submit a NOP batch to RCS to verify the pipeline end-to-end */
    IOReturn    submitNopBatch();

    /* Process context status buffer interrupts */
    void        processCSB(TGLEngineID engine);

    bool        isGucMode() const { return _gucMode; }

private:
    IOReturn    initEngine(TGLEngineID engine);
    IOReturn    initLRC(TGLHwContext *ctx, TGLEngineID engine);
    void        populateLrcRegisters(TGLHwContext *ctx, TGLEngineID engine);
    void        ringEmitNop(TGLHwContext *ctx, uint32_t count);
    void        ringEmitFlush(TGLHwContext *ctx);
    void        ringAdvanceTail(TGLHwContext *ctx);

    FakeIrisXEFramebuffer *  _fb       = nullptr;
    bool                     _gucMode  = false;

    /* Default contexts for each engine (used for NOP test) */
    TGLHwContext *  _defaultCtx[kEngineCount] = {};

    uint32_t    _nextCtxId = 1;
};

#endif /* FAKEIRISXEEXECLIST_HPP */
