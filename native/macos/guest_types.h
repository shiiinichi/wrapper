#pragma once
#include <cstddef>
#include <cstring>
namespace native_poc {
struct GuestShared { void* object = nullptr; void* control = nullptr; };
// Borrowed const-reference view. Guest APIs copy it; never destroy this view.
struct GuestString {
    size_t capacity = 1, size;
    const char* data;
    explicit GuestString(const char* text) : size(std::strlen(text)), data(text) {}
};
static_assert(sizeof(GuestString) == 24 && sizeof(GuestShared) == 16);
}
