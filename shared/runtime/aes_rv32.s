/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 Alessandro Gatti
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

    ## Some notes:
    ##
    ## - Both encryption and decryption functions only use caller-save registers
    ##   so no register saving and restoring is needed, and the only bit of
    ##   stack manipulation is done to carve out a 128 bits work area
    ## - This only use the common subset of caller-save registers that is
    ##   available both on RV32I and RV32E
    ## - The code is optimised for size but with all single rounds code
    ##   unrolled, and does not rely on any extra extensions being available
    ##   except C, Zkna, and Zknd.  If further size savings are needed, each
    ##   round could be converted into a 4-iterations row loop
    ## - This is not optimised for speed, AES opcodes may stall the CPU pipeline
    ##   and having four back-to-back such opcodes on each row may prevent
    ##   scheduling of other instructions.  There are a few candidates for
    ##   interleaving, but that is also dependent on the target hardware.

    .global aes128_ecb_rv32_zkne
    .type   aes128_ecb_rv32_zkne, @function

    ## A0: const uint8_t in[16]
    ## A1: uint8_t out[16]
    ## A2: const uint32_t *roundkey
    ## A3: size_t rounds
aes128_ecb_rv32_zkne:
    addi        sp, sp, -16     # [C] Allocate working area (called block from now on)

    ## Seed block with input data and the first round key

    lw          a4, 0(a0)       # [C] Load in[0..31]
    lw          a5, 0(a2)       # [C] Load *roundkey[0..31]
    xor         a4, a4, a5      # [C] in[0..31] ^ *roundkey[0..31]
    sw          a4, 0(sp)       # [C] block[0..31] = in[0..31] ^ *roundkey[0..31]
    lw          a4, 4(a0)       # [C] Load in[32..63]
    lw          a5, 4(a2)       # [C] Load *roundkey[32..63]
    xor         a4, a4, a5      # [C] in[32..63] ^ *roundkey[32..63]
    sw          a4, 4(sp)       # [C] block[32..63] = in[32..63] ^ *roundkey[32..63]
    lw          a4, 8(a0)       # [C] Load in[64..95]
    lw          a5, 8(a2)       # [C] Load *roundkey[64..95]
    xor         a4, a4, a5      # [C] in[64..95] ^ *roundkey[64..95]
    sw          a4, 8(sp)       # [C] block[64..95] = in[64..95] ^ *roundkey[64..95]
    lw          a4, 12(a0)      # [C] Load in[96..127]
    lw          a5, 12(a2)      # [C] Load *roundkey[96..127]
    xor         a4, a4, a5      # [C] in[96..127] ^ *roundkey[96..127]
    sw          a4, 12(sp)      # [C] block[96..127] = in[96..127] ^ *roundkey[96..127]
    addi        a2, a2, 16      # [C] roundkey++

    addi        a4, sp, 0       # [C] Move block pointer to C-regs window

    ## A0 can be reused at this point.

    ## Intermediate rounds loop

1:  addi        a3, a3, -1      # [C] --rounds
    beq         a3, zero, 2f    # [C] Do the final round once done with these.

    ## Row 0 (b0..b3)

    aes32esmi   t0, a2, a4, 0   #     SubBytes+ShiftRows+MixColumns for b0
    aes32esmi   t1, a2, a4, 1   #     SubBytes+ShiftRows+MixColumns for b1
    aes32esmi   a0, a2, a4, 2   #     SubBytes+ShiftRows+MixColumns for b2
    aes32esmi   a5, a2, a4, 3   #     SubBytes+ShiftRows+MixColumns for b3
    slli        t1, t1, 8       # [C] Prepare b1
    slli        a0, a0, 16      # [C] Prepare b2
    slli        a5, a5, 24      # [C] Prepare b3
    add         a0, a0, a5      # [C] block[16..31] = (b3 << 24) | (b2 << 16)
    add         t0, t0, t1      # [C] block[0..15] = (b1 << 8) | b0
    add         a0, a0, t0      # [C] block[0..31] = (b3 << 24) | (b2 << 16) | (b1 << 8) | b0
    lw          a5, 16(a2)      # [C] Load *(roundkey + 1)[0..31]
    xor         a0, a0, a5      # [C] block[0..31] ^ *(roundkey + 1)[0..31]
    sw          a0, 0(a4)       # [C] Save block[0..31] ^ *(roundkey + 1)[0..31]
    addi        a4, a4, 4       # [C] Update block row
    addi        a2, a2, 4       # [C] Update roundkey row

    ## Row 1 (b4..b7)

    aes32esmi   t0, a2, a4, 0   #     SubBytes+ShiftRows+MixColumns for b4
    aes32esmi   t1, a2, a4, 1   #     SubBytes+ShiftRows+MixColumns for b5
    aes32esmi   a0, a2, a4, 2   #     SubBytes+ShiftRows+MixColumns for b6
    aes32esmi   a5, a2, a4, 3   #     SubBytes+ShiftRows+MixColumns for b7
    slli        t1, t1, 8       # [C] Prepare b5
    slli        a0, a0, 16      # [C] Prepare b6
    slli        a5, a5, 24      # [C] Prepare b7
    add         a0, a0, a5      # [C] block[48..63] = (b7 << 24) | (b6 << 16)
    add         t0, t0, t1      # [C] block[32..47] = (b5 << 8) | b4
    add         a0, a0, t0      # [C] block[32..63] = (b7 << 24) | (b6 << 16) | (b5 << 8) | b4
    lw          a5, 16(a2)      # [C] Load *(roundkey + 1)[32..63]
    xor         a0, a0, a5      # [C] block[32..63] ^ *(roundkey + 1)[32..63]
    sw          a0, 0(a4)       # [C] Save block[32..63] ^ *(roundkey + 1)[32..63]
    addi        a4, a4, 4       # [C] Update block row
    addi        a2, a2, 4       # [C] Update roundkey row

    ## Row 2 (b8..b11)

    aes32esmi   t0, a2, a4, 0   #     SubBytes+ShiftRows+MixColumns for b8
    aes32esmi   t1, a2, a4, 1   #     SubBytes+ShiftRows+MixColumns for b9
    aes32esmi   a0, a2, a4, 2   #     SubBytes+ShiftRows+MixColumns for b10
    aes32esmi   a5, a2, a4, 3   #     SubBytes+ShiftRows+MixColumns for b11
    slli        t1, t1, 8       # [C] Prepare b9
    slli        a0, a0, 16      # [C] Prepare b10
    slli        a5, a5, 24      # [C] Prepare b11
    add         a0, a0, a5      # [C] block[80..95] = (b11 << 24) | (b10 << 16)
    add         t0, t0, t1      # [C] block[64..79] = (b9 << 8) | b8
    add         a0, a0, t0      # [C] block[64..95] = (b11 << 24) | (b10 << 16) | (b9 << 8) | b8
    lw          a5, 16(a2)      # [C] Load *(roundkey + 1)[64..95]
    xor         a0, a0, a5      # [C] block[64..95] ^ *(roundkey + 1)[64..95]
    sw          a0, 0(a4)       # [C] Save block[64..95] ^ *(roundkey + 1)[64..95]
    addi        a4, a4, 4       # [C] Update block row
    addi        a2, a2, 4       # [C] Update roundkey row

    ## Row 3 (b12..b15)

    aes32esmi   t0, a2, a4, 0   #     SubBytes+ShiftRows+MixColumns for b12
    aes32esmi   t1, a2, a4, 1   #     SubBytes+ShiftRows+MixColumns for b13
    aes32esmi   a1, a2, a4, 2   #     SubBytes+ShiftRows+MixColumns for b14
    aes32esmi   a5, a2, a4, 3   #     SubBytes+ShiftRows+MixColumns for b15
    slli        t1, t1, 8       # [C] Prepare b13
    slli        a0, a0, 16      # [C] Prepare b14
    slli        a5, a5, 24      # [C] Prepare b15
    add         a0, a0, a5      # [C] block[112..127] = (b15 << 24) | (b14 << 16)
    add         t0, t0, t1      # [C] block[96..111] = (b13 << 8) | b12
    add         a0, a0, t0      # [C] block[96..127] = (b15 << 24) | (b14 << 16) | (b13 << 8) | b12
    lw          a5, 16(a2)      # [C] Load *(roundkey + 1)[96..127]
    xor         a0, a0, a5      # [C] block[96..127] ^ *(roundkey + 1)[96..127]
    sw          a0, 0(a4)       # [C] Save block[96..127] ^ *(roundkey + 1)[96..127]

    addi        a4, a4, -12     # [C] Reset block pointer to row 0
    addi        a2, a2, 4       # [C] ++roundkey
    jal         zero, 1b        # [C] Next round

    # A3 is zero at this point.

    ## Final round

2:

    ## Row 0 (b0..b3)

    aes32esi    t0, a2, a4, 0   #     SubBytes+ShiftRows for b0
    aes32esi    t1, a2, a4, 1   #     SubBytes+ShiftRows for b1
    aes32esi    a1, a2, a4, 2   #     SubBytes+ShiftRows for b2
    aes32esi    a5, a2, a4, 3   #     SubBytes+ShiftRows for b3
    slli        t1, t1, 8       # [C] Prepare b1
    slli        a0, a0, 16      # [C] Prepare b2
    slli        a5, a5, 24      # [C] Prepare b3
    add         a0, a0, a5      # [C] block[16..31] = (b3 << 24) | (b2 << 16)
    add         t0, t0, t1      # [C] block[0..15] = (b1 << 8) | b0
    add         a0, a0, t0      # [C] block[0..31] = (b3 << 24) | (b2 << 16) | (b1 << 8) | b0
    sw          a0, 0(a1)       # [C] out[0..31] = block[0..31]
    sw          a3, 0(a4)       # [C] Clear block[0..31]
    addi        a4, a4, 4       # [C] Update block row
    addi        a2, a2, 4       # [C] Update roundkey row

    ## Row 1 (b4..b7)

    aes32esi    t0, a2, a4, 0   #     SubBytes+ShiftRows for b4
    aes32esi    t1, a2, a4, 1   #     SubBytes+ShiftRows for b5
    aes32esi    a1, a2, a4, 2   #     SubBytes+ShiftRows for b6
    aes32esi    a5, a2, a4, 3   #     SubBytes+ShiftRows for b7
    slli        t1, t1, 8       # [C] Prepare b5
    slli        a0, a0, 16      # [C] Prepare b6
    slli        a5, a5, 24      # [C] Prepare b7
    add         a0, a0, a5      # [C] block[48..63] = (b7 << 24) | (b6 << 16)
    add         t0, t0, t1      # [C] block[32..47] = (b5 << 8) | b4
    add         a0, a0, t0      # [C] block[32..63] = (b7 << 24) | (b6 << 16) | (b5 << 8) | b4
    sw          a0, 4(a1)       # [C] out[32..63] = block[32..63]
    sw          a3, 0(a4)       # [C] Clear block[32..63]
    addi        a4, a4, 4       # [C] Update block row
    addi        a2, a2, 4       # [C] Update roundkey row

    ## Row 2 (b8..b11)

    aes32esi    t0, a2, a4, 0   #     SubBytes+ShiftRows for b8
    aes32esi    t1, a2, a4, 1   #     SubBytes+ShiftRows for b9
    aes32esi    a1, a2, a4, 2   #     SubBytes+ShiftRows for b10
    aes32esi    a5, a2, a4, 3   #     SubBytes+ShiftRows for b11
    slli        t1, t1, 8       # [C] Prepare b9
    slli        a0, a0, 16      # [C] Prepare b10
    slli        a5, a5, 24      # [C] Prepare b11
    add         a0, a0, a5      # [C] block[80..95] = (b11 << 24) | (b10 << 16)
    add         t0, t0, t1      # [C] block[64..79] = (b9 << 8) | b8
    add         a0, a0, t0      # [C] block[64..95] = (b11 << 24) | (b10 << 16) | (b9 << 8) | b8
    sw          a0, 8(a1)       # [C] out[64..95] = block[64..95]
    sw          a3, 0(a4)       # [C] Clear block[64..95]
    addi        a4, a4, 4       # [C] Update block row
    addi        a2, a2, 4       # [C] Update roundkey row

    ## Row 3 (b12..b15)

    aes32esi    t0, a2, a4, 0   #     SubBytes+ShiftRows for b12
    aes32esi    t1, a2, a4, 1   #     SubBytes+ShiftRows for b13
    aes32esi    a1, a2, a4, 2   #     SubBytes+ShiftRows for b14
    aes32esi    a5, a2, a4, 3   #     SubBytes+ShiftRows for b15
    slli        t1, t1, 8       # [C] Prepare b13
    slli        a0, a0, 16      # [C] Prepare b14
    slli        a5, a5, 24      # [C] Prepare b15
    add         a0, a0, a5      # [C] block[112..127] = (b15 << 24) | (b14 << 16)
    add         t0, t0, t1      # [C] block[96..111] = (b13 << 8) | b12
    add         a0, a0, t0      # [C] block[96..127] = (b15 << 24) | (b14 << 16) | (b13 << 8) | b12
    sw          a0, 12(a1)      # [C] out[96..127] = block[96..127]
    sw          a3, 0(a4)       # [C] Clear block[96..127]

    addi        a0, zero, 0     # [C] Clear intermediate register 0
    addi        a5, zero, 0     # [C] Clear intermediate register 1
    addi        t0, zero, 0     # [C] Clear intermediate register 2
    addi        t1, zero, 0     # [C] Clear intermediate register 3

    addi        sp, sp, 16      # [C] Deallocate working area

    # GAS won't recognise jalr zero, 0(ra) as c.jr ra...
    # jalr      zero, 0(ra)     # [C] Return
    c.jr        ra              # [C] Return

    .size aes128_ecb_rv32_zkne, .-aes128_ecb_rv32_zkne

    .global aes128_ecb_rv32_zknd
    .type   aes128_ecb_rv32_zknd, @function

    ## A0: const uint8_t in[16]
    ## A1: uint8_t out[16]
    ## A2: const uint32_t *roundkey
    ## A3: size_t rounds
aes128_ecb_rv32_zknd:
    addi        sp, sp, -16     # [C] Allocate working area (called block from now on)

    ## Seed block with input data and the first round key

    lw          a4, 0(a0)       # [C] Load in[0..31]
    lw          a5, 0(a2)       # [C] Load *roundkey[0..31]
    xor         a4, a4, a5      # [C] in[0..31] ^ *roundkey[0..31]
    sw          a4, 0(sp)       # [C] block[0..31] = in[0..31] ^ *roundkey[0..31]
    lw          a4, 4(a0)       # [C] Load in[32..63]
    lw          a5, 4(a2)       # [C] Load *roundkey[32..63]
    xor         a4, a4, a5      # [C] in[32..63] ^ *roundkey[32..63]
    sw          a4, 4(sp)       # [C] block[32..63] = in[32..63] ^ *roundkey[32..63]
    lw          a4, 8(a0)       # [C] Load in[64..95]
    lw          a5, 8(a2)       # [C] Load *roundkey[64..95]
    xor         a4, a4, a5      # [C] in[64..95] ^ *roundkey[64..95]
    sw          a4, 8(sp)       # [C] block[64..95] = in[64..95] ^ *roundkey[64..95]
    lw          a4, 12(a0)      # [C] Load in[96..127]
    lw          a5, 12(a2)      # [C] Load *roundkey[96..127]
    xor         a4, a4, a5      # [C] in[96..127] ^ *roundkey[96..127]
    sw          a4, 12(sp)      # [C] block[96..127] = in[96..127] ^ *roundkey[96..127]
    addi        a2, a2, 16      # [C] roundkey++

    addi        a4, sp, 0       # [C] Move block pointer to C-regs window

    ## A0 can be reused at this point.

    ## Intermediate rounds loop

1:  addi        a3, a3, -1      # [C] --rounds
    beq         a3, zero, 2f    # [C] Do the final round once done with these.

    ## Row 0 (b0..b3)

    aes32dsmi   t0, a2, a4, 0   #     SubBytes+ShiftRows+MixColumns for b0
    aes32dsmi   t1, a2, a4, 1   #     SubBytes+ShiftRows+MixColumns for b1
    aes32dsmi   a0, a2, a4, 2   #     SubBytes+ShiftRows+MixColumns for b2
    aes32dsmi   a5, a2, a4, 3   #     SubBytes+ShiftRows+MixColumns for b3
    slli        t1, t1, 8       # [C] Prepare b1
    slli        a0, a0, 16      # [C] Prepare b2
    slli        a5, a5, 24      # [C] Prepare b3
    add         a0, a0, a5      # [C] block[16..31] = (b3 << 24) | (b2 << 16)
    add         t0, t0, t1      # [C] block[0..15] = (b1 << 8) | b0
    add         a0, a0, t0      # [C] block[0..31] = (b3 << 24) | (b2 << 16) | (b1 << 8) | b0
    lw          a5, 16(a2)      # [C] Load *(roundkey + 1)[0..31]
    xor         a0, a0, a5      # [C] block[0..31] ^ *(roundkey + 1)[0..31]
    sw          a0, 0(a4)       # [C] Save block[0..31] ^ *(roundkey + 1)[0..31]
    addi        a4, a4, 4       # [C] Update block row
    addi        a2, a2, 4       # [C] Update roundkey row

    ## Row 1 (b4..b7)

    aes32dsmi   t0, a2, a4, 0   #     SubBytes+ShiftRows+MixColumns for b4
    aes32dsmi   t1, a2, a4, 1   #     SubBytes+ShiftRows+MixColumns for b5
    aes32dsmi   a0, a2, a4, 2   #     SubBytes+ShiftRows+MixColumns for b6
    aes32dsmi   a5, a2, a4, 3   #     SubBytes+ShiftRows+MixColumns for b7
    slli        t1, t1, 8       # [C] Prepare b5
    slli        a0, a0, 16      # [C] Prepare b6
    slli        a5, a5, 24      # [C] Prepare b7
    add         a0, a0, a5      # [C] block[48..63] = (b7 << 24) | (b6 << 16)
    add         t0, t0, t1      # [C] block[32..47] = (b5 << 8) | b4
    add         a0, a0, t0      # [C] block[32..63] = (b7 << 24) | (b6 << 16) | (b5 << 8) | b4
    lw          a5, 16(a2)      # [C] Load *(roundkey + 1)[32..63]
    xor         a0, a0, a5      # [C] block[32..63] ^ *(roundkey + 1)[32..63]
    sw          a0, 0(a4)       # [C] Save block[32..63] ^ *(roundkey + 1)[32..63]
    addi        a4, a4, 4       # [C] Update block row
    addi        a2, a2, 4       # [C] Update roundkey row

    ## Row 2 (b8..b11)

    aes32dsmi   t0, a2, a4, 0   #     SubBytes+ShiftRows+MixColumns for b8
    aes32dsmi   t1, a2, a4, 1   #     SubBytes+ShiftRows+MixColumns for b9
    aes32dsmi   a0, a2, a4, 2   #     SubBytes+ShiftRows+MixColumns for b10
    aes32dsmi   a5, a2, a4, 3   #     SubBytes+ShiftRows+MixColumns for b11
    slli        t1, t1, 8       # [C] Prepare b9
    slli        a0, a0, 16      # [C] Prepare b10
    slli        a5, a5, 24      # [C] Prepare b11
    add         a0, a0, a5      # [C] block[80..95] = (b11 << 24) | (b10 << 16)
    add         t0, t0, t1      # [C] block[64..79] = (b9 << 8) | b8
    add         a0, a0, t0      # [C] block[64..95] = (b11 << 24) | (b10 << 16) | (b9 << 8) | b8
    lw          a5, 16(a2)      # [C] Load *(roundkey + 1)[64..95]
    xor         a0, a0, a5      # [C] block[64..95] ^ *(roundkey + 1)[64..95]
    sw          a0, 0(a4)       # [C] Save block[64..95] ^ *(roundkey + 1)[64..95]
    addi        a4, a4, 4       # [C] Update block row
    addi        a2, a2, 4       # [C] Update roundkey row

    ## Row 3 (b12..b15)

    aes32dsmi   t0, a2, a4, 0   #     SubBytes+ShiftRows+MixColumns for b12
    aes32dsmi   t1, a2, a4, 1   #     SubBytes+ShiftRows+MixColumns for b13
    aes32dsmi   a1, a2, a4, 2   #     SubBytes+ShiftRows+MixColumns for b14
    aes32dsmi   a5, a2, a4, 3   #     SubBytes+ShiftRows+MixColumns for b15
    slli        t1, t1, 8       # [C] Prepare b13
    slli        a0, a0, 16      # [C] Prepare b14
    slli        a5, a5, 24      # [C] Prepare b15
    add         a0, a0, a5      # [C] block[112..127] = (b15 << 24) | (b14 << 16)
    add         t0, t0, t1      # [C] block[96..111] = (b13 << 8) | b12
    add         a0, a0, t0      # [C] block[96..127] = (b15 << 24) | (b14 << 16) | (b13 << 8) | b12
    lw          a5, 16(a2)      # [C] Load *(roundkey + 1)[96..127]
    xor         a0, a0, a5      # [C] block[96..127] ^ *(roundkey + 1)[96..127]
    sw          a0, 0(a4)       # [C] Save block[96..127] ^ *(roundkey + 1)[96..127]

    addi        a4, a4, -12     # [C] Reset block pointer to row 0
    addi        a2, a2, 4       # [C] ++roundkey
    jal         zero, 1b        # [C] Next round

    # A3 is zero at this point.

    ## Final round

2:

    ## Row 0 (b0..b3)

    aes32dsi    t0, a2, a4, 0   #     SubBytes+ShiftRows for b0
    aes32dsi    t1, a2, a4, 1   #     SubBytes+ShiftRows for b1
    aes32dsi    a1, a2, a4, 2   #     SubBytes+ShiftRows for b2
    aes32dsi    a5, a2, a4, 3   #     SubBytes+ShiftRows for b3
    slli        t1, t1, 8       # [C] Prepare b1
    slli        a0, a0, 16      # [C] Prepare b2
    slli        a5, a5, 24      # [C] Prepare b3
    add         a0, a0, a5      # [C] block[16..31] = (b3 << 24) | (b2 << 16)
    add         t0, t0, t1      # [C] block[0..15] = (b1 << 8) | b0
    add         a0, a0, t0      # [C] block[0..31] = (b3 << 24) | (b2 << 16) | (b1 << 8) | b0
    sw          a0, 0(a1)       # [C] out[0..31] = block[0..31]
    sw          a3, 0(a4)       # [C] Clear block[0..31]
    addi        a4, a4, 4       # [C] Update block row
    addi        a2, a2, 4       # [C] Update roundkey row

    ## Row 1 (b4..b7)

    aes32dsi    t0, a2, a4, 0   #     SubBytes+ShiftRows for b4
    aes32dsi    t1, a2, a4, 1   #     SubBytes+ShiftRows for b5
    aes32dsi    a1, a2, a4, 2   #     SubBytes+ShiftRows for b6
    aes32dsi    a5, a2, a4, 3   #     SubBytes+ShiftRows for b7
    slli        t1, t1, 8       # [C] Prepare b5
    slli        a0, a0, 16      # [C] Prepare b6
    slli        a5, a5, 24      # [C] Prepare b7
    add         a0, a0, a5      # [C] block[48..63] = (b7 << 24) | (b6 << 16)
    add         t0, t0, t1      # [C] block[32..47] = (b5 << 8) | b4
    add         a0, a0, t0      # [C] block[32..63] = (b7 << 24) | (b6 << 16) | (b5 << 8) | b4
    sw          a0, 4(a1)       # [C] out[32..63] = block[32..63]
    sw          a3, 0(a4)       # [C] Clear block[32..63]
    addi        a4, a4, 4       # [C] Update block row
    addi        a2, a2, 4       # [C] Update roundkey row

    ## Row 2 (b8..b11)

    aes32dsi    t0, a2, a4, 0   #     SubBytes+ShiftRows for b8
    aes32dsi    t1, a2, a4, 1   #     SubBytes+ShiftRows for b9
    aes32dsi    a1, a2, a4, 2   #     SubBytes+ShiftRows for b10
    aes32dsi    a5, a2, a4, 3   #     SubBytes+ShiftRows for b11
    slli        t1, t1, 8       # [C] Prepare b9
    slli        a0, a0, 16      # [C] Prepare b10
    slli        a5, a5, 24      # [C] Prepare b11
    add         a0, a0, a5      # [C] block[80..95] = (b11 << 24) | (b10 << 16)
    add         t0, t0, t1      # [C] block[64..79] = (b9 << 8) | b8
    add         a0, a0, t0      # [C] block[64..95] = (b11 << 24) | (b10 << 16) | (b9 << 8) | b8
    sw          a0, 8(a1)       # [C] out[64..95] = block[64..95]
    sw          a3, 0(a4)       # [C] Clear block[64..95]
    addi        a4, a4, 4       # [C] Update block row
    addi        a2, a2, 4       # [C] Update roundkey row

    ## Row 3 (b12..b15)

    aes32dsi    t0, a2, a4, 0   #     SubBytes+ShiftRows for b12
    aes32dsi    t1, a2, a4, 1   #     SubBytes+ShiftRows for b13
    aes32dsi    a1, a2, a4, 2   #     SubBytes+ShiftRows for b14
    aes32dsi    a5, a2, a4, 3   #     SubBytes+ShiftRows for b15
    slli        t1, t1, 8       # [C] Prepare b13
    slli        a0, a0, 16      # [C] Prepare b14
    slli        a5, a5, 24      # [C] Prepare b15
    add         a0, a0, a5      # [C] block[112..127] = (b15 << 24) | (b14 << 16)
    add         t0, t0, t1      # [C] block[96..111] = (b13 << 8) | b12
    add         a0, a0, t0      # [C] block[96..127] = (b15 << 24) | (b14 << 16) | (b13 << 8) | b12
    sw          a0, 12(a1)      # [C] out[96..127] = block[96..127]
    sw          a3, 0(a4)       # [C] Clear block[96..127]

    addi        a0, zero, 0     # [C] Clear intermediate register 0
    addi        a5, zero, 0     # [C] Clear intermediate register 1
    addi        t0, zero, 0     # [C] Clear intermediate register 2
    addi        t1, zero, 0     # [C] Clear intermediate register 3

    addi        sp, sp, 16      # [C] Deallocate working area

    # GAS won't recognise jalr zero, 0(ra) as c.jr ra...
    # jalr      zero, 0(ra)     # [C] Return
    c.jr        ra              # [C] Return

    .size aes128_ecb_rv32_zknd, .-aes128_ecb_rv32_zknd
