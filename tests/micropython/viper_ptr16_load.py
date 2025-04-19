# test loading from ptr16 type
# only works on little endian machines


@micropython.viper
def get(src: ptr16) -> int:
    return src[0]


@micropython.viper
def get1(src: ptr16) -> int:
    return src[1]


@micropython.viper
def get_index(src: ptr16, i: int) -> int:
    return src[i]


@micropython.viper
def memadd(src: ptr16, n: int) -> int:
    sum = 0
    for i in range(n):
        sum += src[i]
    return sum


@micropython.viper
def memadd2(src_in) -> int:
    src = ptr16(src_in)
    n = int(len(src_in)) >> 1
    sum = 0
    for i in range(n):
        sum += src[i]
    return sum


b = bytearray(b"1234")
print(b)
print(get(b), get1(b))
print(memadd(b, 2))
print(memadd2(b))

# Test boundary conditions for various architectures

import gc

gc.collect()

b = bytearray(5000)
b[28:38] = b"0123456789"
b[252:262] = b"ABCDEFGHIJ"
b[4092:4102] = b"KLMNOPQRST"

GET_TEMPLATE = """
@micropython.viper
def get{off}(src: ptr16) -> int:
    return src[{off}]
print(b[{off} * 2:({off} + 1) * 2])
"""

for pre, idx, post in (15, 16, 17), (127, 128, 129), (2047, 2048, 2049):
    print(get_index(b, pre), get_index(b, idx), get_index(b, post))
    exec(GET_TEMPLATE.format(off=pre))
    exec(GET_TEMPLATE.format(off=idx))
    exec(GET_TEMPLATE.format(off=post))
