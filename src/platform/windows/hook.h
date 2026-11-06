#pragma once

#include <expected>

namespace s8::platform::windows {

struct Hook {
    void* target;
    void* original;
};

int initialize_hooks();
std::expected<Hook, int> create_hook(void* target, void* detour);
int enable_hook(const Hook& hook);
int disable_hook(const Hook& hook);
// The caller must establish that no callback or trampoline remains in use.
int remove_hook(const Hook& hook);
int uninitialize_hooks();

} // namespace s8::platform::windows
