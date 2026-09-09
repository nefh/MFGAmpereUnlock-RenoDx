// SPDX-License-Identifier: MIT
// Logging double, not the real ReShade SDK.
#pragma once
#include <string>
#include <vector>
namespace reshade::log {
enum class level {info,warning,error};
inline std::vector<std::string> lines;
inline void message(level,const char* text){lines.emplace_back(text);}
}
