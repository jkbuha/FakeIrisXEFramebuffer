/* embedded_firmware.cpp — firmware placeholder
 * Replace with xxd-generated output from tgl_guc_70.bin (see embedded_firmware.h)
 */
#include "embedded_firmware.h"

#if GUC_FIRMWARE_AVAILABLE
/* *** REPLACE THIS SECTION WITH REAL FIRMWARE ***
 * xxd -i i915/tgl_guc_70.bin will generate:
 *   unsigned char tgl_guc_70_bin[] = { 0x..., ... };
 *   unsigned int  tgl_guc_70_bin_len = NNNNN;
 * Rename to gTGLGuCFirmwareData / gTGLGuCFirmwareSize
 */
const uint8_t  gTGLGuCFirmwareData[] = { 0x00 }; /* placeholder — replace */
const size_t   gTGLGuCFirmwareSize   = 0;
#else
const uint8_t  gTGLGuCFirmwareData[] = { 0x00 };
const size_t   gTGLGuCFirmwareSize   = 0;
#endif
