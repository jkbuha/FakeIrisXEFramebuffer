/* FakeIrisXERing.h */
#ifndef FAKEIRISXERING_H
#define FAKEIRISXERING_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
static inline void ring_write32(volatile uint8_t *ringBase, uint32_t *tail,
                                 uint32_t ringSize, uint32_t val) {
    volatile uint32_t *p = (volatile uint32_t *)(ringBase + *tail);
    *p = val;
    *tail = (*tail + 4) % ringSize;
}
static inline void ring_align8(uint32_t *tail) {
    *tail = (*tail + 7) & ~7U;
}
#ifdef __cplusplus
}
#endif
#endif
