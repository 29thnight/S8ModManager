#include "hook.h"

#include "../../../third_party/minhook/include/MinHook.h"

namespace s8::platform::windows {

int initialize_hooks() {
    return static_cast<int>(MH_Initialize());
}

std::expected<Hook, int> create_hook(void* target, void* detour) {
    Hook hook{target, nullptr};
    const auto status = MH_CreateHook(target, detour, &hook.original);
    if (status != MH_OK)
        return std::unexpected(static_cast<int>(status));
    return hook;
}

int enable_hook(const Hook& hook) {
    return static_cast<int>(MH_EnableHook(hook.target));
}

int disable_hook(const Hook& hook) {
    return static_cast<int>(MH_DisableHook(hook.target));
}

int remove_hook(const Hook& hook) {
    return static_cast<int>(MH_RemoveHook(hook.target));
}

int uninitialize_hooks() {
    return static_cast<int>(MH_Uninitialize());
}

} // namespace s8::platform::windows
