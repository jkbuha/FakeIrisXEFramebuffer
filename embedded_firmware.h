/* embedded_firmware.h — GuC firmware embedding for FakeIrisXEFramebuffer.kext
 *
 * Tiger Lake GuC firmware: tgl_guc_70.bin
 * SHA-256: obtain from linux-firmware (linux-firmware/i915/tgl_guc_70.bin)
 *
 * HOW TO EMBED THE REAL FIRMWARE:
 *   1. Clone https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git
 *   2. Copy i915/tgl_guc_70.bin into the project directory
 *   3. Run: xxd -i tgl_guc_70.bin > embedded_firmware.cpp
 *   4. Replace the placeholder array below with the generated output.
 *      Rename the array/length to: gTGLGuCFirmwareData / gTGLGuCFirmwareSize
 *
 * Until the real firmware is embedded, GuC submission is DISABLED and
 * the driver falls back to legacy Execlists (Path B).
 */

#ifndef EMBEDDED_FIRMWARE_H
#define EMBEDDED_FIRMWARE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Set to 1 once the real firmware binary is embedded in embedded_firmware.cpp */
#define GUC_FIRMWARE_AVAILABLE  0

/* Firmware data pointer and size.
 * When GUC_FIRMWARE_AVAILABLE == 0, these are NULL/0 and GuC is skipped. */
extern const uint8_t  gTGLGuCFirmwareData[];
extern const size_t   gTGLGuCFirmwareSize;

/* GuC firmware header (matches Intel GuC ABI, v70) */
#pragma pack(push,1)
typedef struct {
    uint32_t signature;         /* 0x00425443 "CTB\0" for v70 */
    uint32_t header_size;       /* bytes */
    uint32_t major;
    uint32_t minor;
    uint32_t patch;
    uint32_t loader_stage1_size;
    uint32_t ukernel_offset;
    uint32_t ukernel_size;
    uint8_t  rsvd[48];
} GucFirmwareHeader;
#pragma pack(pop)

#ifdef __cplusplus
}
#endif

#endif /* EMBEDDED_FIRMWARE_H */
