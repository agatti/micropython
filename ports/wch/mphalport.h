#include "ch32v003fun.h"

#include "py/obj.h"

void wch_system_init(void);

static inline mp_uint_t mp_hal_ticks_cpu(void) {
    // Not currently implemented.
    return 0;
}

static inline mp_uint_t mp_hal_ticks_ms(void) {
    // Not currently implemented.
    return 0;
}

static inline mp_uint_t mp_hal_ticks_us(void) {
    // Not currently implemented.
    return 0;
}

static inline uint64_t mp_hal_time_ns(void) {
    // Not currently implemented.
    return 0;
}

static inline void mp_hal_delay_ms(mp_uint_t ms) {
    Delay_Ms(ms);
}

static inline void mp_hal_delay_us(mp_uint_t us) {
    Delay_Us(us);
}

static inline void mp_hal_set_interrupt_char(char c) {
    // Not currently implemented.
}
