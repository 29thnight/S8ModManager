#pragma once

#include <cstdint>

namespace s8::domain {

struct GameEpoch {
    std::uint64_t value;
    bool operator==(const GameEpoch&) const = default;
};

struct OfficerId {
    std::uint32_t value;
    bool operator==(const OfficerId&) const = default;
};

struct RequestId {
    std::uint64_t value;
    bool operator==(const RequestId&) const = default;
};

} // namespace s8::domain
