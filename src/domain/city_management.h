#pragma once

#include "ids.h"
#include "ownership.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace s8::domain {

inline constexpr std::size_t CITY_COUNT = 51;
inline constexpr std::size_t CITY_FACTION_COUNT = 120;
inline constexpr std::uint8_t NO_CITY = 0xff;
inline constexpr std::uint16_t NO_CITY_OFFICER = 0xffff;
inline constexpr std::array<std::string_view, CITY_COUNT> CITY_NAMES{
    "양평", "북평", "계", "남피", "업", "평원", "북해", "제남", "진양", "상당", "하비", "소패",
    "복양", "진류", "허창", "초", "여남", "낙양", "홍농", "장안", "천수", "무위", "광릉", "서평",
    "한중", "무도", "성도", "영안", "자동", "강주", "영창", "완", "신야", "양양", "강하", "상용",
    "강릉", "장사", "영릉", "무릉", "계양", "수춘", "여강", "말릉/건업", "오", "회계", "시상", "파양",
    "건녕", "운남", "교지"};

enum class CityField : std::uint8_t {
    Gold, Grain, Troops, DevelopmentLimit, CommerceLimit, DefenseLimit, TechnologyLimit, TroopLimit, Revolt, Count
};

inline constexpr std::size_t CITY_FIELD_COUNT = static_cast<std::size_t>(CityField::Count);
inline constexpr std::array<std::string_view, CITY_FIELD_COUNT> CITY_FIELD_NAMES{
    "금", "군량", "병사", "개발한도", "상업한도", "방어한도", "기술한도", "병사한도", "반란"};

struct CitySettings {
    std::uint32_t grain_limit = 200000;
    std::uint32_t retained_grain = 100000;
    std::uint32_t exchange_rate = 10;
    bool auto_exchange = false;
    bool revolt_always_zero = false;
    bool operator==(const CitySettings&) const = default;
};

enum class CitySupportMode : std::uint8_t { Fixed, Percent };

struct CitySupportRoute {
    std::uint8_t source = NO_CITY;
    std::uint8_t target = NO_CITY;
    CitySupportMode mode = CitySupportMode::Fixed;
    std::uint32_t gold = 0;
    std::uint32_t grain = 0;
    std::uint32_t troops = 0;
    bool enabled = true;
    bool operator==(const CitySupportRoute&) const = default;
};

struct CitySupportRoutes {
    std::vector<CitySupportRoute> routes;
    bool yearly_auto = false;
};

enum class CityAction : std::uint8_t {
    Refresh, Write, Maximize, ResetRevolt, Support, RunRoutes, Cancel,
    RefreshTechnology, WriteTechnology, MaximizeTechnology, ConvertSelected
};

struct CityRequest {
    CityAction action = CityAction::Refresh;
    std::uint8_t city = 0;
    CityField field = CityField::Gold;
    std::uint32_t value = 0;
    std::uint16_t force_lord = NO_CITY_OFFICER;
    std::uint8_t technology = 0;
    CitySupportRoute support;
    std::uint64_t selected_cities = 0;
};

struct SetCitySettings { CitySettings settings; };
struct SetCitySupportRoutes { own::shared_owner<const CitySupportRoutes> routes; };

enum class CityWorkStatus : std::uint8_t {
    Ready, Working, Complete, Cancelled, SessionChanged, InvalidRequest, Busy,
    ReadFailed, WriteFailed, RollbackFailed, OwnershipChanged, NoResources
};

struct CityRow {
    std::array<std::uint32_t, CITY_FIELD_COUNT> values{};
    std::array<std::uint8_t, 6> neighbors{NO_CITY, NO_CITY, NO_CITY, NO_CITY, NO_CITY, NO_CITY};
    std::uint16_t force_lord = NO_CITY_OFFICER;
    std::uint16_t governor = NO_CITY_OFFICER;
    std::uint64_t corps = 0;
    std::uint8_t foreign_neighbors = 0;
    std::uint8_t unknown_connections = 0;
    bool valid = false;
    bool occupied = false;
    bool frontline = false;
};

struct CityFactionTechnology {
    std::uint16_t lord = NO_CITY_OFFICER;
    std::array<std::uint8_t, 5> levels{};
    bool valid = false;
};

inline constexpr std::array<std::uint8_t, 5> CITY_TECHNOLOGY_LIMITS{3, 3, 3, 5, 3};
inline constexpr std::array<std::string_view, 5> CITY_TECHNOLOGY_NAMES{"보병", "기병", "궁병", "병기", "함선"};

struct CitySnapshot {
    GameEpoch epoch{};
    std::uint64_t revision = 0;
    CitySettings settings;
    own::shared_owner<const CitySupportRoutes> routes;
    std::array<CityRow, CITY_COUNT> cities{};
    std::array<CityFactionTechnology, CITY_FACTION_COUNT> factions{};
    CityWorkStatus status = CityWorkStatus::Ready;
    CityWorkStatus last_request = CityWorkStatus::Ready;
    std::uint16_t player_force_lord = NO_CITY_OFFICER;
    std::uint16_t year = 0;
    std::uint8_t month = 0;
    std::size_t completed = 0;
    std::size_t total = 0;
    std::size_t succeeded = 0;
    std::array<std::uint32_t, 3> moved{};
    bool troop_limit_reached = false;
    bool available = false;
    bool busy = false;
    bool locks_commands = false;
    bool technologies_loaded = false;
};

constexpr bool valid_city_settings(const CitySettings& value) noexcept {
    return value.exchange_rate != 0 && value.exchange_rate <= 0x7fffffff &&
        value.grain_limit <= 0x7fffffff && value.retained_grain <= 0x7fffffff;
}

constexpr bool valid_city_route(const CitySupportRoute& route) noexcept {
    return route.source < CITY_COUNT && route.target < CITY_COUNT && route.source != route.target &&
        (route.mode == CitySupportMode::Fixed || (route.mode == CitySupportMode::Percent &&
            route.gold <= 100 && route.grain <= 100 && route.troops <= 100));
}

constexpr std::uint32_t city_field_maximum(CityField field) noexcept {
    switch (field) {
    case CityField::DevelopmentLimit: case CityField::CommerceLimit:
    case CityField::DefenseLimit: case CityField::TechnologyLimit:
        return 30000;
    case CityField::Revolt:
        return 0;
    default:
        return 9999999;
    }
}

} // namespace s8::domain
