"""lz4.block.decompress for the phone's Python, in pure Python.

The recompiler reads the NSO segments with `lz4.block.decompress(blob, uncompressed_size=n)` (recomp/nso.py). The lz4
package is a C extension with no Android wheel, so the APK's Python carries this instead: the LZ4 block format
(https://github.com/lz4/lz4/blob/dev/doc/lz4_Block_format.md), decoding only. A main module of tens of MB decodes in
seconds, once per setup.
"""


class LZ4BlockError(Exception):
    pass


def decompress(source, uncompressed_size=-1, return_bytearray=False, dict=None):  # noqa: A002 (the C API's name)
    src = memoryview(bytes(source))
    if uncompressed_size < 0:  # the C package's default: a 4-byte little-endian size prefix
        uncompressed_size = int.from_bytes(src[:4], 'little')
        src = src[4:]
    out = bytearray()
    i, n = 0, len(src)
    while i < n:
        token = src[i]
        i += 1
        length = token >> 4
        if length == 15:
            while True:
                b = src[i]
                i += 1
                length += b
                if b != 255:
                    break
        out += src[i:i + length]
        i += length
        if i >= n:  # the last sequence has literals only
            break
        offset = src[i] | (src[i + 1] << 8)
        i += 2
        if offset == 0 or offset > len(out):
            raise LZ4BlockError('corrupt input: bad match offset')
        length = token & 15
        if length == 15:
            while True:
                b = src[i]
                i += 1
                length += b
                if b != 255:
                    break
        length += 4
        start = len(out) - offset
        if length <= offset:
            out += out[start:start + length]
        else:  # an overlapping match repeats the last `offset` bytes
            chunk = out[start:]
            whole, rest = divmod(length, offset)
            out += chunk * whole + chunk[:rest]
    if len(out) != uncompressed_size:
        raise LZ4BlockError(f'decompressed {len(out)} bytes, expected {uncompressed_size}')
    return out if return_bytearray else bytes(out)
