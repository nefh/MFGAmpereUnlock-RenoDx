// SPDX-License-Identifier: MIT
#pragma once
// Only the logging sink is replaced; Windows APIs and Detours are real.
namespace reshade::log {
enum class level { error, warning, info, debug };
inline void message(level, const char*) {}
}
