# test loading from ptr32 type


@micropython.viper
def get(src: ptr32) -> int:
    return src[0]


@micropython.viper
def get1(src: ptr32) -> int:
    return src[1]


@micropython.viper
def get_index(src: ptr32, i: int) -> int:
    return src[i]


@micropython.viper
def memadd(src: ptr32, n: int) -> int:
    sum = 0
    for i in range(n):
        sum += src[i]
    return sum


@micropython.viper
def memadd2(src_in) -> int:
    src = ptr32(src_in)
    n = int(len(src_in)) >> 2
    sum = 0
    for i in range(n):
        sum += src[i]
    return sum


b = bytearray(b"\x12\x12\x12\x12\x34\x34\x34\x34")
print(b)
print(hex(get(b)), hex(get1(b)))
print(hex(memadd(b, 2)))
print(hex(memadd2(b)))

# Test boundary conditions for various architectures

import gc

gc.collect()

b = bytearray(5000)
b[24:43] = b"0123456789ABCDEFGHIJ"
b[248:268] = b"KLMNOPQRSTUVWXYZabcd"
b[4088:4108] = b"efghijklmnopqrstuvwx"

GET_TEMPLATE = """
@micropython.viper
def get{off}(src: ptr32) -> int:
    return src[{off}]
print(b[{off} * 4:({off} + 1) * 4])
"""

for pre, idx, post in (7, 8, 9), (63, 64, 65), (1023, 1024, 1025):
    print(get_index(b, pre), get_index(b, idx), get_index(b, post))
    exec(GET_TEMPLATE.format(off=pre))
    exec(GET_TEMPLATE.format(off=idx))
    exec(GET_TEMPLATE.format(off=post))
