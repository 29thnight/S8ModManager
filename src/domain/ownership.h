#pragma once

// Each proxy links the runtime statically. Owners stay inside that DLL; COM and
// Win32 resources keep their native RAII wrappers at the platform boundary.
#include "../../third_party/ownership_cpp/include/own/ownership.hpp"
