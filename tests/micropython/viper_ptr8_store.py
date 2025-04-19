# test ptr8 type


@micropython.viper
def set(dest: ptr8, val: int):
    dest[0] = val


@micropython.viper
def set1(dest: ptr8, val: int):
    dest[1] = val


@micropython.viper
def set_index(dest: ptr8, i: int, val: int):
    dest[i] = val


@micropython.viper
def memset(dest: ptr8, val: int, n: int):
    for i in range(n):
        dest[i] = val


@micropython.viper
def memset2(dest_in, val: int):
    dest = ptr8(dest_in)
    n = int(len(dest_in))
    for i in range(n):
        dest[i] = val


b = bytearray(4)
print(b)

set(b, 41)
print(b)

set1(b, 42)
print(b)

memset(b, 43, len(b))
print(b)

memset2(b, 44)
print(b)

# Test boundary conditions for various architectures

import gc

gc.collect()

b = bytearray(5000)
test_data = ((49, 31, 3), (52, 254, 3), (55, 4094, 3))
for val, start, count in test_data:
    for i in range(count):
        set_index(b, start + i, val + i)
    print(b[start : start + count])

SET_TEMPLATE = """
@micropython.viper
def set{off}(dest: ptr8):
    dest[{off}] = {val}
set{off}(b)
print(b[{off}])
"""

for val, start, count in test_data:
    for i in range(count):
        exec(SET_TEMPLATE.format(off=start + i, val=val + i + 16))
