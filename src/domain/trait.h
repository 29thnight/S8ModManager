#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <map>
#include <optional>

namespace s8::domain {

// OfficerData.h의 TalentEffect와 TalentInfo에서 게임 주소를 제외한 값이다.
struct TraitEffect {
    std::uint16_t effect_id;
    std::uint16_t value1;
    std::uint16_t value2;
};

struct Trait {
    std::uint16_t id;
    std::array<TraitEffect, 6> effects;
};

using OfficerTraits = std::array<std::optional<Trait>, 3>;

struct TraitEffectDefinition {
    std::string text;
    std::unordered_map<int, std::string> values;
    std::unordered_map<int, std::string> params;
};

using TraitEffectDefinitions = std::unordered_map<std::uint16_t, TraitEffectDefinition>;

struct CustomTrait {
    std::string name;
    int grade = 0;
};

using CustomTraits = std::map<std::uint16_t, CustomTrait>;

} // namespace s8::domain
