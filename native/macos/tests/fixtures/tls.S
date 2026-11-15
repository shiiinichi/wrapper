
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

.global sret_probe
.type sret_probe, %function
sret_probe:
    stp x0, x1, [x8]
    stp x2, x3, [x8, #16]
    stp x4, x5, [x8, #32]
    mov x0, #0xdead
    mov x1, #0xbeef
    ret
.size sret_probe, .-sret_probe

.global sret9_probe
.type sret9_probe, %function
sret9_probe:
    stp x0, x1, [x8]
    stp x2, x3, [x8, #16]
    stp x4, x5, [x8, #32]
    stp x6, x7, [x8, #48]
    ldr x9, [sp]
    str x9, [x8, #64]
    ret
.size sret9_probe, .-sret9_probe

.global format_probe
.type format_probe, %function
format_probe:
    sub sp, sp, #64
    stp x29, x30, [sp, #48]
    mov x16, x0
    mov x0, x1
    mov x1, #256
    adr x2, .Lformat
    mov x3, #7
    mov x4, #-9
    mov x5, #0x1234
    adr x6, .Lstring
    mov x7, #66
    fmov d0, #3.25
    mov x9, #0x5678
    movk x9, #0x1234, lsl #16
    str x9, [sp]
    mov x9, #6
    str x9, [sp, #8]
    mov x9, #3
    str x9, [sp, #16]
    adr x9, .Lstring
    str x9, [sp, #24]
    fmov d1, #0.5
    fmov d2, #0.5
    fmov d3, #0.5
    fmov d4, #0.5
    fmov d5, #0.5
    fmov d6, #0.5
    fmov d7, #0.5
    fmov d17, #3.5
    str d17, [sp, #32]
    blr x16
    ldp x29, x30, [sp, #48]
    add sp, sp, #64
    ret
.size format_probe, .-format_probe
.Lformat: .asciz "%d %lld %x %.3s %c %.2f %zu %*.*s %g %g %g %g %g %g %g %g"
.Lstring: .asciz "abcdef"
.p2align 2
.global cf_format_probe
.type cf_format_probe, %function
cf_format_probe:
    sub sp, sp, #64
    stp x29, x30, [sp, #48]
    mov x16, x0
    mov x0, x1
    mov x1, #0
    mov x4, #42
    mov x5, #0x5678
    movk x5, #0x1234, lsl #16
    adr x6, .Lstring
    mov x7, #87
    fmov d0, #3.25
    str x5, [sp]
    mov x9, #6
    str x9, [sp, #8]
    mov x9, #3
    str x9, [sp, #16]
    adr x9, .Lstring
    str x9, [sp, #24]
    blr x16
    ldp x29, x30, [sp, #48]
    add sp, sp, #64
    ret
.size cf_format_probe, .-cf_format_probe
