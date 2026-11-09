// AAPCS64 C++ indirect result: x8 is independent of the named arguments.
// All six arguments here are pointers/64-bit integers. Tail branching preserves
// the caller's LR and lets the guest return directly to the native caller.
.text
.p2align 2
.globl _callGuestReturn
_callGuestReturn:
    mov x16, x0
    mov x8, x1
    mov x0, x2
    mov x1, x3
    mov x2, x4
    mov x3, x5
    mov x4, x6
    mov x5, x7
    br x16

// Capture Linux register-save areas before calling the typed formatter. Darwin
// variadic callees require stack arguments, so direct forwarding is invalid.
.p2align 2
.globl _guestSnprintf
_guestSnprintf:
    mov w9, #0
    b LformatCapture
.globl _guestLogPrint
_guestLogPrint:
    mov w9, #1
LformatCapture:
    sub sp, sp, #224
    stp x29, x30, [sp, #208]
    stp x0, x1, [sp]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp q0, q1, [sp, #64]
    stp q2, q3, [sp, #96]
    stp q4, q5, [sp, #128]
    stp q6, q7, [sp, #160]
    mov x0, sp
    add x1, sp, #64
    add x2, sp, #224
    mov w3, w9
    bl _guestFormatDispatch
    ldp x29, x30, [sp, #208]
    add sp, sp, #224
    ret
