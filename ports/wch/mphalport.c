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
#include "mphalport.h"
#include "wch_platform.h"

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
