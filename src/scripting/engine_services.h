#pragma once
#include "script_api.h"

namespace s8::scripting {
  std::int64_t invoke_engine(Operation operation, std::uint64_t target, std::int64_t argument, void *data,
                             std::uint32_t bytes, std::uint64_t session);
}
