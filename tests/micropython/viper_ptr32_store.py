# test store to ptr32 type


@micropython.viper
def set(dest: ptr32, val: int):
    dest[0] = val


@micropython.viper
def set1(dest: ptr32, val: int):
    dest[1] = val


@micropython.viper
def set_index(dest: ptr32, i: int, val: int):
    dest[i] = val


@micropython.viper
def memset(dest: ptr32, val: int, n: int):
    for i in range(n):
        dest[i] = val


@micropython.viper
def memset2(dest_in, val: int):
    dest = ptr32(dest_in)
    n = int(len(dest_in)) >> 2
    for i in range(n):
        dest[i] = val


b = bytearray(8)
print(b)

set(b, 0x42424242)
print(b)

set1(b, 0x43434343)
print(b)

memset(b, 0x44444444, len(b) // 4)
print(b)

memset2(b, 0x45454545)
print(b)

# Test boundary conditions for various architectures

import gc

gc.collect()

b = bytearray(5000)
test_data = (
    (3, (0x04030201, 0x08070605, 0x0C0B0A09)),
    (63, (0x100F0E0D, 0x14131211, 0x18171615)),
    (1023, (0x1C1B1A19, 0x201F1E1D, 0x24232221)),
)

for start, vals in test_data:
    for i, v in enumerate(vals):
        set_index(b, start + i, v)
        print(b[(start + i) * 4 : (start + i + 1) * 4])

gc.collect()
b = bytearray(5000)

SET_TEMPLATE = """
@micropython.viper
def set{off}(dest: ptr32):
    dest[{off}] = {val} & 0x3FFFFFFF
set{off}(b)
print(b[{off} * 4:({off} + 1) * 4])
"""

for start, vals in test_data:
    for i, v in enumerate(vals):
        exec(SET_TEMPLATE.format(off=start + i, val=v))
