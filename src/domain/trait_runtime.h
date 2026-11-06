#pragma once

#include "trait.h"

#include <array>
#include <cstddef>
#include <string>

namespace s8::domain {

inline constexpr std::size_t TRAIT_RUNTIME_COUNT = 254;

struct TraitRuntimeEntry {
    bool present = false;
    Trait trait{};
};

struct TraitRuntimeMetadata {
    bool present = false;
    std::string name;
    std::string description;
    int clone_source = -1;
    int background_trait = -1;
    int grade = -1;
    int voice = -1;
};

struct TraitRuntimeCatalog {
    std::array<TraitRuntimeEntry, TRAIT_RUNTIME_COUNT> traits{};
    std::array<TraitRuntimeMetadata, TRAIT_RUNTIME_COUNT> metadata{};
};

} // namespace s8::domain
