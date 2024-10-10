/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
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

#ifndef MICROPY_INCLUDED_WCH_WCH_PLATFORM_H
#define MICROPY_INCLUDED_WCH_WCH_PLATFORM_H

#include <stdint.h>

#include "mpconfigport.h"

typedef struct _wch_clock_tree_t {
    mp_uint_t sysclk; // System clock speed.
    mp_uint_t hclk;   // High performance bus clock speed.
    mp_uint_t pclk1;  // Low speed peripheral bus clock speed.
    mp_uint_t pclk2;  // High speed peripheral bus clock speed.
} wch_clock_tree_t;

void get_core_clock_tree_entries(wch_clock_tree_t *tree);

// These are needed because whilst the official WCH BSP has functions
// and constants to cover all possible pin modes, the redistributable
// SDK only contains a subset of those constants.  Since this port
// moved away from the WCH BSP, we have to cover the full constants set
// ourselves.  There's no real point in reusing the same constant values
// as the SDK at this point, so these defines will do the job in the
// meantime.

#define WCH_PIN_INPUT_FLOATING           (0)
#define WCH_PIN_OUTPUT                   (1)
#define WCH_PIN_ANALOG                   (2)
#define WCH_PIN_INVALID_STATE            (3)
#define WCH_PIN_PULL_UP                  (1U << 2)
#define WCH_PIN_PULL_DOWN                (1U << 3)
#define WCH_PIN_ALTERNATE_FUNCTION       (1U << 4)
#define WCH_PIN_OPEN_DRAIN               (1U << 5)
#define WCH_PIN_MODE(value)              ((value) & 0x03)
#define WCH_PIN_PULL_UP_PULL_DOWN(value) ((value) & (WCH_PIN_PULL_UP | WCH_PIN_PULL_DOWN))

#endif // MICROPY_INCLUDED_WCH_WCH_PLATFORM_H
