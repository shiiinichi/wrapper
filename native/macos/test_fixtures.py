"""Independent ELF fixture assembled by Clang for the TLS adaptation tests."""
from pathlib import Path
import struct
import subprocess


def tls_fixture(directory: Path) -> Path:
    source = directory / "tls.S"
    object_file = directory / "tls.o"
    source.write_text(r"""
.text
.p2align 2
.global tls_probe
.type tls_probe, %function
tls_probe:
    stp x29, x30, [sp, #-64]!
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    mov x29, sp
    mrs x20, fpcr
    mrs x21, fpsr
    mov x22, #0x400000
    msr fpcr, x22
    mov x10, #1
    msr fpsr, x10
    mov x19, sp
    mov x0, #0x123
    mov x1, #0x234
    mov x8, #0xdead
    mov x16, #0x456
    mov x17, #0x567
    movi v0.16b, #0x5a
    movi v31.16b, #0x6b
    cmp x0, x0
    mrs x9, tpidr_el0
    b.ne .Lfailed
    cmp x0, #0x123
    b.ne .Lfailed
    cmp x1, #0x234
    b.ne .Lfailed
    mov x10, #0xdead
    cmp x8, x10
    b.ne .Lfailed
    cmp x16, #0x456
    b.ne .Lfailed
    cmp x17, #0x567
    b.ne .Lfailed
    mov x10, sp
    cmp x10, x19
    b.ne .Lfailed
    umov w10, v0.b[0]
    cmp w10, #0x5a
    b.ne .Lfailed
    umov w10, v31.b[15]
    cmp w10, #0x6b
    b.ne .Lfailed
    mrs x10, fpcr
    cmp x10, x22
    b.ne .Lfailed
    mrs x10, fpsr
    cmp x10, #1
    b.ne .Lfailed
    cbz x9, .Lfailed
    ldr x10, [x9]
    cmp x9, x10
    b.ne .Lfailed
    ldr x10, [x9, #40]
    cbz x10, .Lfailed
    mov x0, x9
    b .Ldone
.Lfailed:
    mov x0, #0
.Ldone:
    msr fpcr, x20
    msr fpsr, x21
    ldp x21, x22, [sp, #32]
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #64
    ret
.size tls_probe, .-tls_probe
""")
    subprocess.run(["xcrun", "clang", "--target=aarch64-linux-android22", "-c", str(source),
                    "-o", str(object_file)], check=True, capture_output=True)
    obj = object_file.read_bytes()
    shoff = struct.unpack_from("<Q", obj, 40)[0]
    shsize, shnum = struct.unpack_from("<HH", obj, 58)
    sections = [struct.unpack_from("<IIQQQQIIQQ", obj, shoff + i * shsize) for i in range(shnum)]
    text = next(s for s in sections if s[1] == 1 and s[2] & 4)
    code = obj[text[4]:text[4] + text[5]]
    data = bytearray(0x2900)
    ident = b"\x7fELF\x02\x01\x01" + b"\0" * 9
    struct.pack_into("<16sHHIQQQIHHHHHH", data, 0, ident, 3, 183, 1, 0, 64, 0x2800,
                     0, 64, 56, 3, 64, 3, 0)
    headers = [(1, 5, 0, 0, 0, 0x1000 + len(code), 0x1000 + len(code), 4096),
               (1, 6, 0x2000, 0x10000, 0, 0x400, 0x400, 4096),
               (2, 6, 0x2100, 0x10100, 0, 96, 96, 8)]
    for i, header in enumerate(headers):
        struct.pack_into("<IIQQQQQQ", data, 64 + i * 56, *header)
    data[0x1000:0x1000 + len(code)] = code
    strings = b"\0tls_probe\0"
    data[0x2020:0x2020 + len(strings)] = strings
    struct.pack_into("<IBBHQQ", data, 0x2060 + 24, 1, 0x12, 0, 1, 0x1000, len(code))
    struct.pack_into("<IIIII", data, 0x20A0, 1, 2, 1, 0, 0)
    for i, (tag, value) in enumerate([(5, 0x10020), (10, len(strings)), (6, 0x10060),
                                     (11, 24), (4, 0x100A0), (0, 0)]):
        struct.pack_into("<qQ", data, 0x2100 + i * 16, tag, value)
    for index, section in enumerate([(0, 1, 6, 0x1000, 0x1000, len(code), 0, 0, 4, 0),
                                     (0, 1, 3, 0x10000, 0x2000, 0x400, 0, 0, 8, 0)], 1):
        struct.pack_into("<IIQQQQIIQQ", data, 0x2800 + index * 64, *section)
    output = directory / "tls-fixture.so"
    output.write_bytes(data)
    return output
