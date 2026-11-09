#pragma once
// Only for nontrivial Android C++ result types that use the hidden x8 pointer.
// Each named argument must be a pointer/64-bit scalar; no floats or stack args.
extern "C" void callGuestReturn(void* function, void* result, const void* a0 = nullptr,
    const void* a1 = nullptr, const void* a2 = nullptr, const void* a3 = nullptr,
    const void* a4 = nullptr, const void* a5 = nullptr);
