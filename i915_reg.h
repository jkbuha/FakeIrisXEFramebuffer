/* i915_reg.h — Tiger Lake / Iris Xe register definitions
 * Derived from Linux i915 driver (Intel PRM Vol 2c/12/15/16/17)
 * For use in FakeIrisXEFramebuffer.kext — macOS kernel extension
 *
 * MMIO base: BAR0 (typically 0x4000000000 on TGL, runtime via IOPCIDevice)
 * All offsets relative to MMIO base unless noted.
 */

#ifndef I915_REG_H
#define I915_REG_H

#include <stdint.h>

/* -----------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------- */
#define _MMIO(x)        ((uint32_t)(x))
#define REG_BIT(x)      (1U << (x))
#define REG_GENMASK(h,l) (((~0U) >> (31-(h))) & (~0U << (l)))

/* -----------------------------------------------------------------------
 * FORCEWAKE — GT power domain wakeup
 * --------------------------------------------------------------------- */
#define FORCEWAKE_MT                    _MMIO(0xA188)
#define FORCEWAKE_ACK_HSW               _MMIO(0x130044)
#define FORCEWAKE_ACK_MEDIA_GEN9        _MMIO(0x0D88)
#define FORCEWAKE_ACK_RENDER_GEN9       _MMIO(0x0D84)
#define FORCEWAKE_RENDER_GEN9           _MMIO(0xA278)
#define FORCEWAKE_MEDIA_GEN9            _MMIO(0xA270)
#define FORCEWAKE_MEDIA_VDBOX_GEN11(n)  _MMIO(0xA540 + (n)*4)
#define FORCEWAKE_MEDIA_VEBOX_GEN11(n)  _MMIO(0xA560 + (n)*4)
#define FORCEWAKE_GT_GEN9               _MMIO(0xA188)
#define FORCEWAKE_ACK_GT_GEN9           _MMIO(0x130044)

#define FORCEWAKE_KERNEL                REG_BIT(0)
#define FORCEWAKE_ACK_TIMEOUT_MS        50

/* -----------------------------------------------------------------------
 * GT power wells
 * --------------------------------------------------------------------- */
#define HSW_PWR_WELL_CTL1               _MMIO(0x45400)
#define HSW_PWR_WELL_CTL2               _MMIO(0x45404)
#define HSW_PWR_WELL_CTL_BIOS(pw)       _MMIO(0x45400 + (pw)*8)
#define HSW_PWR_WELL_CTL_DRIVER(pw)     _MMIO(0x45404 + (pw)*8)

#define   HSW_PWR_WELL_CTL_REQ(pw)      REG_BIT((pw)*2 + 1)
#define   HSW_PWR_WELL_CTL_STATE(pw)    REG_BIT((pw)*2)

/* TGL power well IDs */
#define TGL_PW_CTL_IDX_PW_1            14
#define TGL_PW_CTL_IDX_PW_2            13
#define TGL_PW_CTL_IDX_DC_OFF          9
#define TGL_PW_CTL_IDX_DDI_A           0
#define TGL_PW_CTL_IDX_DDI_B           1
#define TGL_PW_CTL_IDX_AUX_A           21
#define TGL_PW_CTL_IDX_AUX_B           22

/* -----------------------------------------------------------------------
 * Display clock / CDCLK
 * --------------------------------------------------------------------- */
#define CDCLK_CTL                       _MMIO(0x46000)
#define   CDCLK_FREQ_SEL_MASK           REG_GENMASK(27,26)
#define   CDCLK_FREQ_337_308            (0 << 26)
#define   CDCLK_FREQ_540               (1 << 26)
#define   CDCLK_FREQ_675_617           (3 << 26)

/* -----------------------------------------------------------------------
 * Pipe / transcoder / plane registers
 * --------------------------------------------------------------------- */

/* Pipe stride / size / position */
#define _PIPEA_STRIDE                   0x70188
#define _PIPEB_STRIDE                   0x71188
#define PIPE_STRIDE(pipe)               _MMIO(_PIPEA_STRIDE + (pipe)*0x1000)

#define _PIPEACONF                      0x70008
#define _PIPEBCONF                      0x71008
#define PIPECONF(pipe)                  _MMIO(_PIPEACONF + (pipe)*0x1000)
#define   PIPECONF_ENABLE               REG_BIT(31)
#define   PIPECONF_STATE_ENABLE         REG_BIT(30)
#define   PIPECONF_BPC_MASK             REG_GENMASK(7,5)
#define   PIPECONF_8BPC                 (0 << 5)
#define   PIPECONF_10BPC               (1 << 5)

/* Transcoder */
#define _TRANS_HTOTAL_A                 0x60000
#define TRANS_HTOTAL(trans)             _MMIO(_TRANS_HTOTAL_A + (trans)*0x1000)
#define _TRANS_HBLANK_A                 0x60004
#define TRANS_HBLANK(trans)             _MMIO(_TRANS_HBLANK_A + (trans)*0x1000)
#define _TRANS_HSYNC_A                  0x60008
#define TRANS_HSYNC(trans)              _MMIO(_TRANS_HSYNC_A + (trans)*0x1000)
#define _TRANS_VTOTAL_A                 0x6000C
#define TRANS_VTOTAL(trans)             _MMIO(_TRANS_VTOTAL_A + (trans)*0x1000)
#define _TRANS_VBLANK_A                 0x60010
#define TRANS_VBLANK(trans)             _MMIO(_TRANS_VBLANK_A + (trans)*0x1000)
#define _TRANS_VSYNC_A                  0x60014
#define TRANS_VSYNC(trans)              _MMIO(_TRANS_VSYNC_A + (trans)*0x1000)

#define _TRANSACONF                     0xF0008
#define TRANSCONF(trans)                _MMIO(_TRANSACONF + (trans)*0x1000)
#define   TRANS_ENABLE                  REG_BIT(31)
#define   TRANS_STATE_ENABLE            REG_BIT(30)

/* Primary plane */
#define _DSPACNTR                       0x70180
#define DSPCNTR(pipe)                   _MMIO(_DSPACNTR + (pipe)*0x1000)
#define   DISPLAY_PLANE_ENABLE          REG_BIT(31)
#define   DISPPLANE_BGRX888             (0x6 << 26)
#define   DISPPLANE_RGBX888             (0x7 << 26)

#define _DSPALINOFF                     0x70184
#define DSPLINOFF(pipe)                 _MMIO(_DSPALINOFF + (pipe)*0x1000)

#define _DSPASURF                       0x7019C
#define DSPSURF(pipe)                   _MMIO(_DSPASURF + (pipe)*0x1000)

#define _DSPASTRIDE                     0x70188
#define DSPSTRIDE(pipe)                 _MMIO(_DSPASTRIDE + (pipe)*0x1000)

#define _DSPASIZE                       0x70190
#define DSPSIZE(pipe)                   _MMIO(_DSPASIZE + (pipe)*0x1000)

/* TGL universal plane (ICL+) */
#define PLANE_CTL(pipe, plane)          _MMIO(0x70180 + (pipe)*0x1000 + (plane)*0x200)
#define   PLANE_CTL_ENABLE              REG_BIT(31)
#define   PLANE_CTL_FORMAT_XRGB_8888    (4 << 24)
#define   PLANE_CTL_PIPE_GAMMA_ENABLE   REG_BIT(22)
#define   PLANE_CTL_PLANE_GAMMA_DISABLE REG_BIT(13)

#define PLANE_STRIDE(pipe, plane)       _MMIO(0x70188 + (pipe)*0x1000 + (plane)*0x200)
#define PLANE_SIZE(pipe, plane)         _MMIO(0x70190 + (pipe)*0x1000 + (plane)*0x200)
#define PLANE_SURF(pipe, plane)         _MMIO(0x7019C + (pipe)*0x1000 + (plane)*0x200)
#define PLANE_OFFSET(pipe, plane)       _MMIO(0x701A4 + (pipe)*0x1000 + (plane)*0x200)

/* -----------------------------------------------------------------------
 * eDP / PSR
 * --------------------------------------------------------------------- */
#define DP_A                            _MMIO(0x64000)
#define   DP_PORT_EN                    REG_BIT(31)
#define   DP_PIPEA_MST_EN              REG_BIT(26)

#define EDP_PSR_CTL                     _MMIO(0x64900)
#define   EDP_PSR_ENABLE                REG_BIT(31)

/* -----------------------------------------------------------------------
 * GGTT (Global GTT)
 * --------------------------------------------------------------------- */
#define GEN8_GGTT_PTE_VALID             (1ULL << 0)
#define GEN8_GGTT_PTE_LM               (1ULL << 1)      /* local memory */
#define GEN8_PAGE_SIZE                  4096UL
#define GGTT_TOTAL_SIZE                 (4ULL * 1024 * 1024 * 1024) /* 4GB aperture */

/* GTT stolen memory base (from graphics stolen base register) */
#define BDSM                            _MMIO(0x1080C0)  /* Base Data of Stolen Memory */
#define BGSM                            _MMIO(0x1080C4)  /* Base GTT Stolen Memory */

/* -----------------------------------------------------------------------
 * Render engine — command streamer
 * --------------------------------------------------------------------- */
#define RENDER_RING_BASE                0x02000
#define BLT_RING_BASE                   0x22000
#define GEN6_BSD_RING_BASE              0x12000
#define VEBOX_RING_BASE                 0x1A000

#define RING_TAIL(base)                 _MMIO((base) + 0x30)
#define RING_HEAD(base)                 _MMIO((base) + 0x34)
#define RING_START(base)                _MMIO((base) + 0x38)
#define RING_CTL(base)                  _MMIO((base) + 0x3C)
#define   RING_CTL_SIZE(sz)             ((sz) - PAGE_SIZE)   /* in pages minus 1 */
#define   RING_VALID                    0x1

#define RING_MI_MODE(base)              _MMIO((base) + 0x9C)
#define RING_MODE_GEN7(base)            _MMIO((base) + 0x29C)
#define   GFX_MODE_ENABLE_PPGTT         (1 << 9)
#define   GFX_MODE_ENABLE_EXECLIST      (1 << 15)

/* Execlist submit port (Gen8+) */
#define RING_ELSP(base)                 _MMIO((base) + 0x230)
#define RING_EXECLIST_STATUS_LO(base)   _MMIO((base) + 0x234)
#define RING_EXECLIST_STATUS_HI(base)   _MMIO((base) + 0x234 + 4)
#define RING_EXECLIST_SQ_CONTENTS(base) _MMIO((base) + 0x510)
#define RING_EXECLIST_CONTROL(base)     _MMIO((base) + 0x550)
#define   EL_CTRL_LOAD                  REG_BIT(0)

/* Context status buffer */
#define RING_CONTEXT_STATUS_BUF_LO(base, i)  _MMIO((base) + 0x370 + (i)*8)
#define RING_CONTEXT_STATUS_BUF_HI(base, i)  _MMIO((base) + 0x374 + (i)*8)
#define RING_CONTEXT_STATUS_PTR(base)         _MMIO((base) + 0x3A8)
#define   GEN8_CSB_ENTRIES                    12
#define   GEN8_CSB_PTR_MASK                   0x7
#define   GEN8_CSB_STATUS_COMPLETE            REG_BIT(4)
#define   GEN8_CSB_STATUS_IDLE                REG_BIT(3)

/* -----------------------------------------------------------------------
 * Logical Ring Context (LRC) — per-context state
 * --------------------------------------------------------------------- */
/* Context offsets within LRC image (in dwords, × 4 = byte offset) */
#define CTX_CONTEXT_CONTROL             0x02   /* dword offset */
#define CTX_RING_HEAD                   0x04
#define CTX_RING_TAIL                   0x06
#define CTX_RING_START                  0x08
#define CTX_RING_CTL                    0x0A
#define CTX_BB_STATE                    0x10
#define CTX_PDP0_UDW                    0x24
#define CTX_PDP0_LDW                    0x26

#define   CONTEXT_CONTROL_INHIBIT_SYN   REG_BIT(3)
#define   CONTEXT_CONTROL_ENGINE_CTX_RESTORE REG_BIT(0)
#define   CONTEXT_CONTROL_RS_CTX_ENABLE REG_BIT(1)

/* LRC image size */
#define GEN11_LRC_PPHWSP_SIZE           0x1000      /* 4 KB per-process HW status page */
#define GEN11_LRC_STATE_SIZE            0x1000      /* 4 KB context state */
#define GEN11_LRC_SIZE                  (GEN11_LRC_PPHWSP_SIZE + GEN11_LRC_STATE_SIZE)

/* -----------------------------------------------------------------------
 * GuC
 * --------------------------------------------------------------------- */
#define GUC_WOPCM_SIZE                  _MMIO(0xC050)
#define GUC_WOPCM_OFFSET                _MMIO(0xC054)
#define   GUC_WOPCM_VALID               REG_BIT(0)
#define   GUC_WOPCM_GFXPAUSE_OFFSET_SHIFT  6
#define   GUC_WOPCM_SIZE_SHIFT          12

#define DMA_CTRL                        _MMIO(0xC3C0)
#define   WOPCM_ENABLE                  REG_BIT(8)
#define   GUC_ENABLE_MIA_CLOCK_GATING   REG_BIT(15)
#define   GUC_ENABLE_MIA_CACHING        REG_BIT(15)

#define GUC_SHIM_CONTROL                _MMIO(0xC064)
#define   GUC_DISABLE_SRAM_INIT_TO_ZEROES REG_BIT(14)
#define   GUC_ENABLE_READ_CACHE_LOGIC   REG_BIT(13)
#define   GUC_ENABLE_MIA_CACHING2       REG_BIT(12)
#define   GUC_ENABLE_PREFETCH_MINOR     REG_BIT(11)
#define   GUC_ENABLE_READ_CACHE_FOR_SRAM_DATA REG_BIT(9)
#define   GUC_ENABLE_READ_CACHE_FOR_WOPCM_DATA REG_BIT(8)
#define   GUC_ENABLE_GUC2HOST_INTERRUPT REG_BIT(4)
#define   GUC_ENABLE_HOST2GUC_DOORBELL  REG_BIT(0)

#define HUC_KERNEL_LOAD_INFO            _MMIO(0xC1C0)
#define GUC_RSTCTL                      _MMIO(0xC010)
#define GUC_WOPCM_OFFSET_VALUE          0x15000       /* 84 KB from WOPCM base */
#define GUC_FIRMWARE_SIZE_MAX           0x80000       /* 512 KB max */

/* GuC CTB (Command Transport Buffer) */
#define GUC_CT_BASE                     _MMIO(0xC300)
#define GUC_LOG_CTL                     _MMIO(0xC340)
#define   GUC_LOG_VALID                 REG_BIT(0)

/* -----------------------------------------------------------------------
 * Interrupt registers
 * --------------------------------------------------------------------- */
#define GEN8_GT_IER(n)                  _MMIO(0x44318 + (n)*4)
#define GEN8_GT_IIR(n)                  _MMIO(0x44308 + (n)*4)
#define GEN8_GT_IMR(n)                  _MMIO(0x44314 + (n)*4)
#define GEN8_GT_ISR(n)                  _MMIO(0x44300 + (n)*4)

#define GEN11_RENDER_COPY_INTR_ENABLE   _MMIO(0x190030)
#define GEN11_VCS_VECS_INTR_ENABLE      _MMIO(0x190034)
#define GEN11_GUC_SG_INTR_ENABLE        _MMIO(0x190038)

/* -----------------------------------------------------------------------
 * Memory / LMEM
 * --------------------------------------------------------------------- */
#define GEN12_GLOBAL_MOCS(i)            _MMIO(0x4000 + (i)*4)

/* -----------------------------------------------------------------------
 * Misc GT
 * --------------------------------------------------------------------- */
#define GEN7_FF_THREAD_MODE             _MMIO(0x20A0)
#define GEN9_CSFE_CHICKEN1_RCS          _MMIO(0x20D4)
#define GEN8_ROW_CHICKEN                _MMIO(0xE4F0)
#define GEN9_ROW_CHICKEN4               _MMIO(0xE48C)
#define GEN7_COMMON_SLICE_CHICKEN1      _MMIO(0x7010)
#define GEN9_HALF_SLICE_CHICKEN7        _MMIO(0xE194)
#define GEN10_SAMPLER_MODE              _MMIO(0xE18C)
#define CHICKEN_RASTER_1                _MMIO(0xE3A0)
#define CHICKEN_RASTER_2                _MMIO(0xE3A4)
#define GEN12_AUX_NV                    _MMIO(0x4DC8)

#define MISCCPCTL                       _MMIO(0x9824)
#define   DOP_CLOCK_GATE_GUC_ENABLE     REG_BIT(0)

/* -----------------------------------------------------------------------
 * Device IDs — Tiger Lake
 * --------------------------------------------------------------------- */
#define INTEL_TGL_IDS \
    0x9A49, /* Iris Xe (96 EU) — i7-1165G7  */ \
    0x9A40, /* Iris Xe (80 EU) — i5-1135G7  */ \
    0x9A59, /* Iris Xe (96 EU) — i7-1185G7  */ \
    0x9A60, /* UHD (32 EU)                   */ \
    0x9A68, /* UHD (48 EU)                   */ \
    0x9A70, /* UHD (32 EU)                   */ \
    0x9A78  /* UHD (24 EU)                   */

#define INTEL_DEVID_TGL_IRIS_XE_96EU    0x9A49
#define INTEL_DEVID_TGL_IRIS_XE_80EU    0x9A40
#define INTEL_DEVID_TGL_IRIS_XE_96EU_H  0x9A59

/* -----------------------------------------------------------------------
 * Page size helpers
 * --------------------------------------------------------------------- */
#ifndef PAGE_SIZE
#define PAGE_SIZE       4096UL
#endif
#ifndef PAGE_SHIFT
#define PAGE_SHIFT      12
#endif

#endif /* I915_REG_H */
