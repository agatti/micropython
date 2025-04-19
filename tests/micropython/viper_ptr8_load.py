# test loading from ptr8 type


@micropython.viper
def get(src: ptr8) -> int:
    return src[0]


@micropython.viper
def get1(src: ptr8) -> int:
    return src[1]


@micropython.viper
def get_index(src: ptr8, i: int) -> int:
    return src[i]


@micropython.viper
def memadd(src: ptr8, n: int) -> int:
    sum = 0
    for i in range(n):
        sum += src[i]
    return sum


@micropython.viper
def memadd2(src_in) -> int:
    src = ptr8(src_in)
    n = int(len(src_in))
    sum = 0
    for i in range(n):
        sum += src[i]
    return sum


b = bytearray(b"1234")
print(b)
print(get(b), get1(b))
print(memadd(b, 4))
print(memadd2(b))

# Test boundary conditions for various architectures

import gc

gc.collect()

b = bytearray(5000)
b[30:32] = b"123"
b[254:256] = b"456"
b[4094:4096] = b"789"

GET_TEMPLATE = """
@micropython.viper
def get{off}(src: ptr8) -> int:
    return src[{off}]
print(get{off}(b))
"""

for pre, idx, post in (30, 31, 32), (254, 255, 256), (4094, 4095, 4096):
    print(get_index(b, pre), get_index(b, idx), get_index(b, post))
    exec(GET_TEMPLATE.format(off=pre))
    exec(GET_TEMPLATE.format(off=idx))
    exec(GET_TEMPLATE.format(off=post))
