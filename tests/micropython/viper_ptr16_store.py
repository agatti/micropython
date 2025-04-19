# test ptr16 type


@micropython.viper
def set(dest: ptr16, val: int):
    dest[0] = val


@micropython.viper
def set1(dest: ptr16, val: int):
    dest[1] = val


@micropython.viper
def set_index(dest: ptr16, i: int, val: int):
    dest[i] = val


@micropython.viper
def memset(dest: ptr16, val: int, n: int):
    for i in range(n):
        dest[i] = val


@micropython.viper
def memset2(dest_in, val: int):
    dest = ptr16(dest_in)
    n = int(len(dest_in)) >> 1
    for i in range(n):
        dest[i] = val


b = bytearray(4)
print(b)

set(b, 0x4242)
print(b)

set1(b, 0x4343)
print(b)

memset(b, 0x4444, len(b) // 2)
print(b)

memset2(b, 0x4545)
print(b)

# Test boundary conditions for various architectures

import gc

gc.collect()

b = bytearray(5000)
test_data = (
    (15, (0x4241, 0x4443, 0x4645)),
    (127, (0x4847, 0x4A49, 0x4C4B)),
    (2047, (0x4E4D, 0x504F, 0x5251)),
)

for start, vals in test_data:
    for i, v in enumerate(vals):
        set_index(b, start + i, v)
        print(b[(start + i) * 2 : (start + i + 1) * 2])

gc.collect()
b = bytearray(5000)

SET_TEMPLATE = """
@micropython.viper
def set{off}(dest: ptr16):
    dest[{off}] = {val}
set{off}(b)
print(b[{off} * 2:({off} + 1) * 2])
"""

for start, vals in test_data:
    for i, v in enumerate(vals):
        exec(SET_TEMPLATE.format(off=start + i, val=v))
