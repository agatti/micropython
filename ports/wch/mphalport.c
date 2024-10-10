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

#include <assert.h>

#include "ch32v003fun.h"
#include "mpconfigport.h"

#define MICROPY_PY_UART1 (1)
#define MICROPY_PY_UART2 (2)
#define MICROPY_PY_UART3 (3)
#define MICROPY_PY_UART4 (4)
#define MICROPY_PY_UART5 (5)
#define MICROPY_PY_UART6 (6)
#define MICROPY_PY_UART7 (7)
#define MICROPY_PY_UART8 (8)

#if MICROPY_PY_CONSOLE_UART_PORT == MICROPY_PY_UART1
#define MICROPY_CONSOLE_UART       (USART1)
#define MICROPY_CONSOLE_PERIPHERAL (RCC_APB2Periph_USART1)
#elif MICROPY_PY_CONSOLE_UART_PORT == MICROPY_PY_UART2
#define MICROPY_CONSOLE_UART       (USART2)
#define MICROPY_CONSOLE_PERIPHERAL (RCC_APB2Periph_USART2)
#elif MICROPY_PY_CONSOLE_UART_PORT == MICROPY_PY_UART3
#define MICROPY_CONSOLE_UART       (USART3)
#define MICROPY_CONSOLE_PERIPHERAL (RCC_APB2Periph_USART3)
#elif MICROPY_PY_CONSOLE_UART_PORT == MICROPY_PY_UART4
#define MICROPY_CONSOLE_UART       (UART4)
#define MICROPY_CONSOLE_PERIPHERAL (RCC_APB2Periph_UART4)
#elif MICROPY_PY_CONSOLE_UART_PORT == MICROPY_PY_UART5
#define MICROPY_CONSOLE_UART       (UART5)
#define MICROPY_CONSOLE_PERIPHERAL (RCC_APB2Periph_UART5)
#elif MICROPY_PY_CONSOLE_UART_PORT == MICROPY_PY_UART6
#define MICROPY_CONSOLE_UART       (UART6)
#define MICROPY_CONSOLE_PERIPHERAL (RCC_APB2Periph_UART6)
#elif MICROPY_PY_CONSOLE_UART_PORT == MICROPY_PY_UART7
#define MICROPY_CONSOLE_UART       (UART7)
#define MICROPY_CONSOLE_PERIPHERAL (RCC_APB2Periph_UART7)
#elif MICROPY_PY_CONSOLE_UART_PORT == MICROPY_PY_UART8
#define MICROPY_CONSOLE_UART       (UART8)
#define MICROPY_CONSOLE_PERIPHERAL (RCC_APB2Periph_UART8)
#else
#error Invalid console UART port definition.
#endif

typedef struct _wch_clock_tree_t {
    mp_uint_t sysclk; // System clock speed.
    mp_uint_t hclk;   // High performance bus clock speed.
    mp_uint_t pclk1;  // Low speed peripheral bus clock speed.
    mp_uint_t pclk2;  // High speed peripheral bus clock speed.
} wch_clock_tree_t;

static mp_uint_t get_sysclk_frequency(void) {
    mp_uint_t sysclk_source = RCC->CFGR0 & CFGR0_SWS_Mask;
    if (sysclk_source == RCC_SWS_HSI) {
        // Internal high speed oscillator.
        return HSI_Value;
    }
    if (sysclk_source == RCC_SWS_HSE) {
        // External high speed oscillator.
        return HSE_Value;
    }
    if (sysclk_source != RCC_SWS_PLL) {
        assert(!"RCC SWS bits are set to an unexpected value.");
        return 0;
    }

    // Compute the frequency from the clock tree.

    mp_uint_t computed_sysclk_frequency = 0;

    // FIXME: The multipliers table is specific for the CH32V307 and other
    //        compatible MCUs (CH32F20x_D8C, CH32V30x_D8C, and CH32V31x_D8C).
    //        This needs to be updated if/when other MCUs from the CH32V line
    //        are added - see RCC_CFGR0's PLLMUL bits documentation for more
    //        information.

    // Represented as 7.1 fixed point to save ROM space.
    static const uint8_t PLL_MULTIPLIERS[] = {
        36, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 13, 30, 32
    };
    mp_uint_t pll_multiplier = PLL_MULTIPLIERS[RCC->CFGR0 & CFGR0_PLLMull_Mask];
    mp_uint_t pll_source = RCC->CFGR0 & CFGR0_PLLSRC_Mask;

    // FIXME: The meaning of these bits is specific for the CH32V307 and other
    //        compatible MCUs (CH32F20x_D8C, CH32V30x_D8C, and CH32V31x_D8C).
    //        This needs to be updated if/when other MCUs from the CH32V line
    //        are added - see RCC_CFGR0's PLLSRC bits documentation for more
    //        information.

    if (pll_source == RCC_PLLSRC_HSI_Div2) {
        // HSI not divided or divided by 2 fed to PLL.
        computed_sysclk_frequency = (EXTEN->EXTEN_CTR & EXTEN_PLL_HSI_PRE) ? HSI_VALUE : (HSI_VALUE >> 1);
    } else {
        // Check PREDIV1 source, whether is HSE or PLL2.
        if (RCC->CFGR2 & CFGR2_PREDIV1SRC) {
            // PLL2 is the PREDIV1 source.

            // 0b0000 as the PREDIV2 divisor value means the clock frequency
            // is not divided at all, hence the bias by 1 to prevent a
            // division by zero.
            computed_sysclk_frequency = HSE_Value / (((RCC->CFGR2 & RCC_PREDIV2_MASK) >> RCC_PREDIV2_OFFSET) + 1);

            // Represented as 7.1 fixed point to save ROM space.
            static const uint8_t PLL2_MULTIPLIERS[] = {
                5, 25, 7, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30, 32, 40
            };
            computed_sysclk_frequency = (computed_sysclk_frequency * PLL2_MULTIPLIERS[(RCC->CFGR2 & RCC_PLL2MUL_MASK) >> RCC_PLL2MUL_OFFSET]) >> 1;
        } else {
            // HSE, divided by PREDIV1.
            computed_sysclk_frequency = HSE_Value / (((RCC->CFGR2 & RCC_PREDIV1_MASK) >> RCC_PREDIV1_OFFSET) + 1);
        }
    }

    return (computed_sysclk_frequency * pll_multiplier) >> 1;
}

static void get_core_clock_tree_entries(wch_clock_tree_t *tree) {
    assert(tree && "Clock tree pointer is NULL.");

    tree->sysclk = get_sysclk_frequency();

    // See HPRE, PPRE1 and PPRE2 bits in RCC_CFGR0.
    static const uint8_t AHB_FREQUENCY_PRESCALING_SHIFTS[] = {
        0, 0, 0, 0, 1, 2, 3, 4, 1, 2, 3, 4, 6, 7, 8, 9
    };

    tree->hclk = tree->sysclk >> AHB_FREQUENCY_PRESCALING_SHIFTS[(RCC->CFGR0 & CFGR0_HPRE_Set_Mask) >> 4];
    tree->pclk1 = tree->hclk >> AHB_FREQUENCY_PRESCALING_SHIFTS[(RCC->CFGR0 & CFGR0_PPRE1_Set_Mask) >> 8];
    tree->pclk2 = tree->pclk1 >> AHB_FREQUENCY_PRESCALING_SHIFTS[(RCC->CFGR0 & CFGR0_PPRE2_Set_Mask) >> 11];
}

// Initialise the serial port for the MicroPython REPL.
static void initialise_console(void) {
    wch_clock_tree_t clock_tree = { 0 };
    get_core_clock_tree_entries(&clock_tree);

    // Set one stop bit.

    uint32_t uart_register = MICROPY_CONSOLE_UART->CTLR2;
    uart_register &= ~USART_CTLR2_STOP;
    uart_register |= USART_StopBits_1;
    MICROPY_CONSOLE_UART->CTLR2 = (uint16_t)uart_register;

    // Set 8 bits, no parity, and bidirectional operation.

    uart_register = MICROPY_CONSOLE_UART->CTLR1;
    uart_register &= USART_CTLR1_UE | USART_CTLR1_WAKE | USART_CTLR1_TXEIE | \
        USART_CTLR1_TCIE | USART_CTLR1_RXNEIE | USART_CTLR1_RWU | USART_CTLR1_SBK;
    uart_register |= USART_WordLength_8b | USART_Parity_No | USART_Mode_Rx | USART_Mode_Tx;
    MICROPY_CONSOLE_UART->CTLR1 = (uint16_t)uart_register;

    // TODO: Is no flow control correct?

    // Set no control flow.

    uart_register = MICROPY_CONSOLE_UART->CTLR3;
    uart_register &= ~(USART_CTLR3_CTSE | USART_CTLR3_RTSE);
    uart_register |= USART_HardwareFlowControl_None;
    MICROPY_CONSOLE_UART->CTLR3 = (uint16_t)uart_register;

    // Set 115200 bauds.

    /*
      18.3 Baud Rate Generator

      The baud rate of the transceiver = FCLK/(16*USARTDIV); FCLK is the clock
      of PBx, i.e., PCLK1 or PCLK2, PCLK2 is used for the USART1 module, and
      PCLK1 shall be used for the rest. The value of USARTDIV is determined
      according to the 2 domains: DIV_M and DIV_F in USART_BRR. The specific
      calculation formula is: USARTDIV = DIV_M+(DIV_F/16)

      TL;DR: the rate is in 12.4 fixed point.
    */

    mp_uint_t clock = (MICROPY_PY_CONSOLE_UART_PORT == MICROPY_PY_UART1) ? clock_tree.pclk2 : clock_tree.pclk1;
    uart_register = ((clock << 4) * (1U << 4)) / (115200 << 4);
    MICROPY_CONSOLE_UART->BRR = (uint16_t)uart_register;
}

void wch_system_init(void) {
    initialise_console();
}

void mp_hal_stdout_tx_strn(const char *string, mp_uint_t length) {
    for (mp_uint_t offset = 0; offset < length; offset++) {
        while ((MICROPY_CONSOLE_UART->STATR & USART_STATR_TXE) == 0) {
        }
        MICROPY_CONSOLE_UART->DATAR = (uint16_t)string[offset];
    }
}

uint16_t mp_hal_stdin_rx_chr(void) {
    while ((MICROPY_CONSOLE_UART->STATR & USART_STATR_RXNE) == 0) {
    }
    return MICROPY_CONSOLE_UART->DATAR & USART_DATAR_DR;
}
