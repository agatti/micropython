/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2013, 2014 Damien P. George
 * Copyright (c) 2022 Rakesh Peter
 * Copyright (c) 2024 Alessandro Gatti
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "py/obj.h"

#include "machine_pin.h"
#include "wch_platform.h"

// Ref: CH32FV2x_V3x Reference Manual - Chapter 10 - GPIO and Alternate Function (GPIO/AFIO)

void gpio_clock_enable(const pin_obj_t *pin) {
    static const uint32_t rcc_map[] = { 0x04, 0x08, 0x10, 0x20, 0x40 };   // RCC_APB2Periph_GPIOA..GPIOE
    RCC->APB2PCENR |= rcc_map[pin->port];
}

uint8_t pin_read(const pin_obj_t *pin) {
    return ((uint8_t)(pin->gpio->INDR & pin->pin_mask) == 0) ? 0 : 1;
}

void pin_write(const pin_obj_t *pin, bool val) {
    if (val) {
        pin->gpio->BSHR = pin->pin_mask;
    } else {
        pin->gpio->BCR = pin->pin_mask;
    }
}
