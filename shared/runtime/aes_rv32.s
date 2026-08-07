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
    ## - This makes use of the full caller-save registers set, so it won't work
    ##   on RV32E (not that is be a problem right now)
    ## - The code is not really optimised for size, and does not rely on any
    ##   extra extensions being available except Zkna, and Zknd
    ## - Some more register reshuffling could have been done to maximise usage
    ##   of compressed opcodes, but that's an optimisation that can be made
    ##   later once this is tested working
    ## - It is not fully optimised for speed either, AES opcodes may stall the
    ##   CPU pipeline and having four back-to-back such opcodes on each row may
    ##   prevent scheduling of other instructions.  There are a few candidates
    ##   for interleaving, but that is also dependent on the target hardware.

    .global aes128_ecb_rv32_zkne
    .type   aes128_ecb_rv32_zkne, @function

    ## A0: const uint8_t in[16]
    ## A1: uint8_t out[16]
    ## A2: const uint32_t *roundkey
    ## A3: size_t rounds
aes128_ecb_rv32_zkne:
    addi        a4, sp, -16     # Allocate working area (called block from now on)

    ## Seed block with input data and the first round key

    lw          a5, 0(a0)       # Load in[0..31]
    lw          a6, 4(a0)       # Load in[32..63]
    lw          a7, 8(a0)       # Load in[64..95]
    lw          t0, 12(a0)      # Load in[96..127]
    lw          t1, 0(a2)       # Load *roundkey[0..31]
    lw          t2, 4(a2)       # Load *roundkey[32..63]
    lw          t3, 8(a2)       # Load *roundkey[64..95]
    lw          t4, 12(a2)      # Load *roundkey[96..127]
    xor         t5, a5, t1      # in[0..31] ^ *roundkey[0..31]
    xor         t6, a6, t2      # in[32..63] ^ *roundkey[32..63]
    sw          t5, 0(a4)       # block[0..31] = in[0..31] ^ *roundkey[0..31]
    sw          t6, 4(a4)       # block[32..63] = in[32..63] ^ *roundkey[32..63]
    xor         t5, a7, t3      # in[64..95] ^ *roundkey[64..95]
    xor         t6, a0, t4      # in[96..127] ^ *roundkey[96..127]
    sw          t5, 8(a4)       # block[64..95] ^ *roundkey[64..95]
    sw          t6, 12(a4)      # block[96..127] ^ *roundkey[96..127]
    addi        a2, a2, 16      # roundkey++

    ## Doing four byte stores and one word load to compose the round word takes
    ## less space than composing a 32-bits word out of four bytes.  We cannot
    ## rely on Zbkb opcodes being available, because otherwise of doing that
    ## we could have done this instead:
    ##
    ## packh h0, b0, b1
    ## packh h1, b2, b3
    ## pack  w0, h0, h1
    ##
    ## saving two instructions for a total of 8 bytes.  Also, that won't touch
    ## memory five times!  This can be reworked later if more speed is needed
    ## than what the crypto opcodes can provide.

    ## Intermediate rounds loop

1:  addi        a3, a3, -1      # --rounds
    beq         a3, zero, 2f    # Do the final round once done with these.

    ## Row 0 (b0..b3)

    aes32esmi   a6, a2, a4, 0   # SubBytes+ShiftRows+MixColumns for b0
    aes32esmi   a7, a2, a4, 1   # SubBytes+ShiftRows+MixColumns for b1
    aes32esmi   t0, a2, a4, 2   # SubBytes+ShiftRows+MixColumns for b2
    aes32esmi   t1, a2, a4, 3   # SubBytes+ShiftRows+MixColumns for b3
    sb          a6, 0(a4)       # block[0..7] = b0
    sb          a7, 1(a4)       # block[8..15] = b1
    sb          t0, 2(a4)       # block[16..23] = b2
    sb          t1, 3(a4)       # block[24..31] = b3
    lw          a5, 0(a4)       # Load block[0..31]
    lw          a6, 16(a2)      # Load *(roundkey + 1)[0..31]
    xor         a5, a5, a6      # block[0..31] ^ *(roundkey + 1)[0..31]
    sw          a6, 0(a4)       # Save block[0..31] ^ *(roundkey + 1)[0..31]
    addi        a4, a4, 4       # Update block row
    addi        a2, a2, 4       # Update roundkey row

    ## Row 1 (b4..b7)

    aes32esmi   a6, a2, a4, 0   # SubBytes+ShiftRows+MixColumns for b4
    aes32esmi   a7, a2, a4, 1   # SubBytes+ShiftRows+MixColumns for b5
    aes32esmi   t0, a2, a4, 2   # SubBytes+ShiftRows+MixColumns for b6
    aes32esmi   t1, a2, a4, 3   # SubBytes+ShiftRows+MixColumns for b7
    sb          a6, 0(a4)       # block[32..39] = b4
    sb          a7, 1(a4)       # block[40..47] = b5
    sb          t0, 2(a4)       # block[48..55] = b6
    sb          t1, 3(a4)       # block[56..63] = b3
    lw          a5, 0(a4)       # Load block[32..63]
    lw          a6, 20(a2)      # Load *(roundkey + 1)[32..63]
    xor         a5, a5, a6      # block[32..63] ^ *(roundkey + 1)[32..63]
    sw          a6, 0(a4)       # Save block[32..63] ^ *(roundkey + 1)[32..63]
    addi        a4, a4, 4       # Update block row
    addi        a2, a2, 4       # Update roundkey row

    ## Row 2 (b8..b11)

    aes32esmi   a6, a2, a4, 0   # SubBytes+ShiftRows+MixColumns for b8
    aes32esmi   a7, a2, a4, 1   # SubBytes+ShiftRows+MixColumns for b9
    aes32esmi   t0, a2, a4, 2   # SubBytes+ShiftRows+MixColumns for b10
    aes32esmi   t1, a2, a4, 3   # SubBytes+ShiftRows+MixColumns for b11
    sb          a6, 0(a4)       # block[64..71] = b8
    sb          a7, 1(a4)       # block[72..79] = b9
    sb          t0, 2(a4)       # block[80..87] = b10
    sb          t1, 3(a4)       # block[88..95] = b11
    lw          a5, 0(a4)       # Load block[64..95]
    lw          a6, 24(a2)      # Load *(roundkey + 1)[64..95]
    xor         a5, a5, a6      # block[64..95] ^ *(roundkey + 1)[64..95]
    sw          a6, 0(a4)       # Save block[64..95] ^ *(roundkey + 1)[64..95]
    addi        a4, a4, 4       # Update block row
    addi        a2, a2, 4       # Update roundkey row

    ## Row 3 (b12..b15)

    aes32esmi   a6, a2, a4, 0   # SubBytes+ShiftRows+MixColumns for b12
    aes32esmi   a7, a2, a4, 1   # SubBytes+ShiftRows+MixColumns for b13
    aes32esmi   t0, a2, a4, 2   # SubBytes+ShiftRows+MixColumns for b14
    aes32esmi   t1, a2, a4, 3   # SubBytes+ShiftRows+MixColumns for b15
    sb          a6, 0(a4)       # block[96..103] = b12
    sb          a7, 1(a4)       # block[104..111] = b13
    sb          t0, 2(a4)       # block[112..119] = b14
    sb          t1, 3(a4)       # block[120..127] = b15
    lw          a5, 0(a4)       # Load block[96..127]
    lw          a6, 28(a2)      # Load *(roundkey + 1)[96..127]
    xor         a5, a5, a6      # block[96..127] ^ *(roundkey + 1)[96..127]
    sw          a6, 0(a4)       # Save block[96..127] ^ *(roundkey + 1)[96..127]

    addi        a4, a4, -12     # Reset block pointer to row 0
    addi        a2, a2, 4       # ++roundkey
    jal         zero, 1b        # Next round

    ## Final round

2:

    ## Row 0 (b0..b3)

    aes32esi    a5, a2, a4, 0   # SubBytes+ShiftRows for b0
    aes32esi    a6, a2, a4, 1   # SubBytes+ShiftRows for b1
    aes32esi    a7, a2, a4, 2   # SubBytes+ShiftRows for b2
    aes32esi    t0, a2, a4, 3   # SubBytes+ShiftRows for b3
    sb          a5, 0(a1)       # out[0..7] = b0
    sb          a6, 1(a1)       # out[8..15] = b1
    sb          a7, 2(a1)       # out[16..23] = b2
    sb          t0, 3(a1)       # out[24..31] = b7
    sw          zero, 0(a4)     # Clear block[0..31]
    addi        a4, a4, 4       # Update block row
    addi        a2, a2, 4       # Update roundkey row

    ## Row 1 (b4..b7)

    aes32esi    a5, a2, a4, 0   # SubBytes+ShiftRows for b4
    aes32esi    a6, a2, a4, 1   # SubBytes+ShiftRows for b5
    aes32esi    a7, a2, a4, 2   # SubBytes+ShiftRows for b6
    aes32esi    t0, a2, a4, 3   # SubBytes+ShiftRows for b7
    sb          a5, 4(a1)       # out[32..39] = b4
    sb          a6, 5(a1)       # out[40..47] = b5
    sb          a7, 6(a1)       # out[48..55] = b6
    sb          t0, 7(a1)       # out[56..63] = b7
    sw          zero, 0(a4)     # Clear block[32..63]
    addi        a4, a4, 4       # Update block row
    addi        a2, a2, 4       # Update roundkey row

    ## Row 2 (b8..b11)

    aes32esi    a5, a2, a4, 0   # SubBytes+ShiftRows for b8
    aes32esi    a6, a2, a4, 1   # SubBytes+ShiftRows for b9
    aes32esi    a7, a2, a4, 2   # SubBytes+ShiftRows for b10
    aes32esi    t0, a2, a4, 3   # SubBytes+ShiftRows for b11
    sb          a5, 8(a1)       # out[64..71] = b8
    sb          a6, 9(a1)       # out[72..79] = b9
    sb          a7, 10(a1)      # out[80..87] = b10
    sb          t0, 11(a1)      # out[88..95] = b11
    sw          zero, 0(a4)     # Clear block[64..95]
    addi        a4, a4, 4       # Update block row
    addi        a2, a2, 4       # Update roundkey row

    ## Row 3 (b12..b15)

    aes32esi    a5, a2, a4, 0   # SubBytes+ShiftRows for b12
    aes32esi    a6, a2, a4, 1   # SubBytes+ShiftRows for b13
    aes32esi    a7, a2, a4, 2   # SubBytes+ShiftRows for b14
    aes32esi    t0, a2, a4, 3   # SubBytes+ShiftRows for b15
    sb          a5, 12(a1)      # out[96..103] = b12
    sb          a6, 13(a1)      # out[104..111] = b13
    sb          a7, 14(a1)      # out[112..119] = b14
    sb          t0, 15(a1)      # out[120..127] = b15
    sw          zero, 0(a4)     # Clear block[96..127]

    addi        sp, sp, 16      # Deallocate working area
    jalr        zero, ra, 0     # Return

    .size aes128_ecb_rv32_zkne, .-aes128_ecb_rv32_zkne

    .global aes128_ecb_rv32_zknd
    .type   aes128_ecb_rv32_zknd, @function

    ## A0: const uint8_t in[16]
    ## A1: uint8_t out[16]
    ## A2: const uint32_t *roundkey
    ## A3: size_t rounds
aes128_ecb_rv32_zknd:
    addi        a4, sp, -16     # Allocate working area (called block from now on)

    ## Seed block with input data and the first round key

    lw          a5, 0(a0)       # Load in[0..31]
    lw          a6, 4(a0)       # Load in[32..63]
    lw          a7, 8(a0)       # Load in[64..95]
    lw          t0, 12(a0)      # Load in[96..127]
    lw          t1, 0(a2)       # Load *roundkey[0..31]
    lw          t2, 4(a2)       # Load *roundkey[32..63]
    lw          t3, 8(a2)       # Load *roundkey[64..95]
    lw          t4, 12(a2)      # Load *roundkey[96..127]
    xor         t5, a5, t1      # in[0..31] ^ *roundkey[0..31]
    xor         t6, a6, t2      # in[32..63] ^ *roundkey[32..63]
    sw          t5, 0(a4)       # block[0..31] = in[0..31] ^ *roundkey[0..31]
    sw          t6, 4(a4)       # block[32..63] = in[32..63] ^ *roundkey[32..63]
    xor         t5, a7, t3      # in[64..95] ^ *roundkey[64..95]
    xor         t6, a0, t4      # in[96..127] ^ *roundkey[96..127]
    sw          t5, 8(a4)       # block[64..95] ^ *roundkey[64..95]
    sw          t6, 12(a4)      # block[96..127] ^ *roundkey[96..127]
    addi        a2, a2, 16      # roundkey++

    ## Doing four byte stores and one word load to compose the round word takes
    ## less space than composing a 32-bits word out of four bytes.  We cannot
    ## rely on Zbkb opcodes being available, because otherwise of doing that
    ## we could have done this instead:
    ##
    ## packh h0, b0, b1
    ## packh h1, b2, b3
    ## pack  w0, h0, h1
    ##
    ## saving two instructions for a total of 8 bytes.  Also, that won't touch
    ## memory five times!  This can be reworked later if more speed is needed
    ## than what the crypto opcodes can provide.

    ## Intermediate rounds loop

1:  addi        a3, a3, -1      # --rounds
    beq         a3, zero, 2f    # Do the final round once done with these.

    ## Row 0 (b0..b3)

    aes32dsmi   a6, a2, a4, 0   # SubBytes+ShiftRows+MixColumns for b0
    aes32dsmi   a7, a2, a4, 1   # SubBytes+ShiftRows+MixColumns for b1
    aes32dsmi   t0, a2, a4, 2   # SubBytes+ShiftRows+MixColumns for b2
    aes32dsmi   t1, a2, a4, 3   # SubBytes+ShiftRows+MixColumns for b3
    sb          a6, 0(a4)       # block[0..7] = b0
    sb          a7, 1(a4)       # block[8..15] = b1
    sb          t0, 2(a4)       # block[16..23] = b2
    sb          t1, 3(a4)       # block[24..31] = b3
    lw          a5, 0(a4)       # Load block[0..31]
    lw          a6, 16(a2)      # Load *(roundkey + 1)[0..31]
    xor         a5, a5, a6      # block[0..31] ^ *(roundkey + 1)[0..31]
    sw          a6, 0(a4)       # Save block[0..31] ^ *(roundkey + 1)[0..31]
    addi        a4, a4, 4       # Update block row
    addi        a2, a2, 4       # Update roundkey row

    ## Row 1 (b4..b7)

    aes32dsmi   a6, a2, a4, 0   # SubBytes+ShiftRows+MixColumns for b4
    aes32dsmi   a7, a2, a4, 1   # SubBytes+ShiftRows+MixColumns for b5
    aes32dsmi   t0, a2, a4, 2   # SubBytes+ShiftRows+MixColumns for b6
    aes32dsmi   t1, a2, a4, 3   # SubBytes+ShiftRows+MixColumns for b7
    sb          a6, 0(a4)       # block[32..39] = b4
    sb          a7, 1(a4)       # block[40..47] = b5
    sb          t0, 2(a4)       # block[48..55] = b6
    sb          t1, 3(a4)       # block[56..63] = b7
    lw          a5, 0(a4)       # Load block[32..63]
    lw          a6, 20(a2)      # Load *(roundkey + 1)[32..63]
    xor         a5, a5, a6      # block[32..63] ^ *(roundkey + 1)[32..63]
    sw          a6, 0(a4)       # Save block[32..63] ^ *(roundkey + 1)[32..63]
    addi        a4, a4, 4       # Update block row
    addi        a2, a2, 4       # Update roundkey row

    ## Row 2 (b8..b11)

    aes32dsmi   a6, a2, a4, 0   # SubBytes+ShiftRows+MixColumns for b8
    aes32dsmi   a7, a2, a4, 1   # SubBytes+ShiftRows+MixColumns for b9
    aes32dsmi   t0, a2, a4, 2   # SubBytes+ShiftRows+MixColumns for b10
    aes32dsmi   t1, a2, a4, 3   # SubBytes+ShiftRows+MixColumns for b11
    sb          a6, 0(a4)       # block[64..71] = b8
    sb          a7, 1(a4)       # block[72..79] = b9
    sb          t0, 2(a4)       # block[80..87] = b10
    sb          t1, 3(a4)       # block[88..95] = b11
    lw          a5, 0(a4)       # Load block[64..95]
    lw          a6, 24(a2)      # Load *(roundkey + 1)[64..95]
    xor         a5, a5, a6      # block[64..95] ^ *(roundkey + 1)[64..95]
    sw          a6, 0(a4)       # Save block[64..95] ^ *(roundkey + 1)[64..95]
    addi        a4, a4, 4       # Update block row
    addi        a2, a2, 4       # Update roundkey row

    ## Row 3 (b12..b15)

    aes32dsmi   a6, a2, a4, 0   # SubBytes+ShiftRows+MixColumns for b12
    aes32dsmi   a7, a2, a4, 1   # SubBytes+ShiftRows+MixColumns for b13
    aes32dsmi   t0, a2, a4, 2   # SubBytes+ShiftRows+MixColumns for b14
    aes32dsmi   t1, a2, a4, 3   # SubBytes+ShiftRows+MixColumns for b15
    sb          a6, 0(a4)       # block[96..103] = b12
    sb          a7, 1(a4)       # block[104..111] = b13
    sb          t0, 2(a4)       # block[112..119] = b14
    sb          t1, 3(a4)       # block[120..127] = b15
    lw          a5, 0(a4)       # Load block[96..127]
    lw          a6, 28(a2)      # Load *(roundkey + 1)[96..127]
    xor         a5, a5, a6      # block[96..127] ^ *(roundkey + 1)[96..127]
    sw          a6, 0(a4)       # Save block[96..127] ^ *(roundkey + 1)[96..127]

    addi        a4, a4, -12     # Reset block pointer to row 0
    addi        a2, a2, 4       # ++roundkey
    jal         zero, 1b        # Next round

    ## Final round

2:

    ## Row 0 (b0..b3)

    aes32dsi    a5, a2, a4, 0   # SubBytes+ShiftRows for b0
    aes32dsi    a6, a2, a4, 1   # SubBytes+ShiftRows for b1
    aes32dsi    a7, a2, a4, 2   # SubBytes+ShiftRows for b2
    aes32dsi    t0, a2, a4, 3   # SubBytes+ShiftRows for b3
    sb          a5, 0(a1)       # out[0..7] = b0
    sb          a6, 1(a1)       # out[8..15] = b1
    sb          a7, 2(a1)       # out[16..23] = b2
    sb          t0, 3(a1)       # out[24..31] = b3
    sw          zero, 0(a4)     # Clear block[0..31]
    addi        a4, a4, 4       # Update block row
    addi        a2, a2, 4       # Update roundkey row

    ## Row 1 (b4..b7)

    aes32dsi    a5, a2, a4, 0   # SubBytes+ShiftRows for b4
    aes32dsi    a6, a2, a4, 1   # SubBytes+ShiftRows for b5
    aes32dsi    a7, a2, a4, 2   # SubBytes+ShiftRows for b6
    aes32dsi    t0, a2, a4, 3   # SubBytes+ShiftRows for b7
    sb          a5, 4(a1)       # out[32..39] = b4
    sb          a6, 5(a1)       # out[40..47] = b5
    sb          a7, 6(a1)       # out[48..55] = b6
    sb          t0, 7(a1)       # out[56..63] = b7
    sw          zero, 0(a4)     # Clear block[32..63]
    addi        a4, a4, 4       # Update block row
    addi        a2, a2, 4       # Update roundkey row

    ## Row 2 (b8..b11)

    aes32dsi    a5, a2, a4, 0   # SubBytes+ShiftRows for b8
    aes32dsi    a6, a2, a4, 1   # SubBytes+ShiftRows for b9
    aes32dsi    a7, a2, a4, 2   # SubBytes+ShiftRows for b10
    aes32dsi    t0, a2, a4, 3   # SubBytes+ShiftRows for b11
    sb          a5, 8(a1)       # out[64..71] = b8
    sb          a6, 9(a1)       # out[72..79] = b9
    sb          a7, 10(a1)      # out[80..87] = b10
    sb          t0, 11(a1)      # out[88..95] = b11
    sw          zero, 0(a4)     # Clear block[64..95]
    addi        a4, a4, 4       # Update block row
    addi        a2, a2, 4       # Update roundkey row

    ## Row 3 (b12..b15)

    aes32dsi    a5, a2, a4, 0   # SubBytes+ShiftRows for b12
    aes32dsi    a6, a2, a4, 1   # SubBytes+ShiftRows for b13
    aes32dsi    a7, a2, a4, 2   # SubBytes+ShiftRows for b14
    aes32dsi    t0, a2, a4, 3   # SubBytes+ShiftRows for b15
    sb          a5, 12(a1)      # out[96..103] = b12
    sb          a6, 13(a1)      # out[104..111] = b13
    sb          a7, 14(a1)      # out[112..119] = b14
    sb          t0, 15(a1)      # out[120..127] = b15
    sw          zero, 0(a4)     # Clear block[96..127]

    addi        sp, sp, 16      # Deallocate working area
    jalr        zero, ra, 0     # Return

    .size aes128_ecb_rv32_zknd, .-aes128_ecb_rv32_zknd
