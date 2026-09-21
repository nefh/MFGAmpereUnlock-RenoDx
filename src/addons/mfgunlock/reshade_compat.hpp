#pragma once

#include <array>
#include <cstddef>

#if defined(MFGUNLOCK_RESHADE_HEADER_OVERRIDE)
#include <reshade.hpp>
#else
#include <include/reshade.hpp>
#endif

#if defined(RESHADE_API_VERSION)
#ifndef MFGUNLOCK_RESHADE_API
#define MFGUNLOCK_RESHADE_API RESHADE_API_VERSION
#endif

static_assert(
    RESHADE_API_VERSION == MFGUNLOCK_RESHADE_API,
    "MFGAmpereUnlock was compiled with ReShade headers that do not match MFGUNLOCK_RESHADE_API");
#endif

// Build helper normally detects these from the selected ReShade checkout. The
// fallbacks keep normal RenoDX builds deterministic when no override is used.
#ifndef MFGUNLOCK_RESHADE_LOG_STYLE
#if defined(RESHADE_API_VERSION) && RESHADE_API_VERSION >= 14
#define MFGUNLOCK_RESHADE_LOG_STYLE 2
#elif defined(RESHADE_API_VERSION) && RESHADE_API_VERSION >= 6
#define MFGUNLOCK_RESHADE_LOG_STYLE 1
#else
#define MFGUNLOCK_RESHADE_LOG_STYLE 0
#endif
#endif

#ifndef MFGUNLOCK_RESHADE_CONFIG_STYLE
#if defined(RESHADE_API_VERSION) && RESHADE_API_VERSION >= 8
#define MFGUNLOCK_RESHADE_CONFIG_STYLE 1
#else
#define MFGUNLOCK_RESHADE_CONFIG_STYLE 0
#endif
#endif

#ifndef MFGUNLOCK_RESHADE_HAS_CONFIG_ARRAY
#if defined(RESHADE_API_VERSION) && RESHADE_API_VERSION >= 18
#define MFGUNLOCK_RESHADE_HAS_CONFIG_ARRAY 1
#else
#define MFGUNLOCK_RESHADE_HAS_CONFIG_ARRAY 0
#endif
#endif

#ifndef MFGUNLOCK_RESHADE_HAS_COLOR_SPACE
#if defined(RESHADE_API_VERSION) && RESHADE_API_VERSION >= 10
#define MFGUNLOCK_RESHADE_HAS_COLOR_SPACE 1
#else
#define MFGUNLOCK_RESHADE_HAS_COLOR_SPACE 0
#endif
#endif

#if MFGUNLOCK_RESHADE_LOG_STYLE < 2
namespace reshade {
namespace log {

enum class level {
  error = 1,
  warning = 2,
  info = 3,
  debug = 4,
};

inline void message(level severity, const char* text) {
#if MFGUNLOCK_RESHADE_LOG_STYLE == 1
  reshade::log_message(static_cast<reshade::log_level>(static_cast<int>(severity)), text);
#else
  reshade::log_message(static_cast<int>(severity), text);
#endif
}

}  // namespace log
}  // namespace reshade
#endif

#if MFGUNLOCK_RESHADE_CONFIG_STYLE == 0
namespace reshade {

inline bool get_config_value(api::effect_runtime* runtime, const char* section,
                             const char* key, char* value, size_t* value_size) {
  if (value != nullptr) {
    return config_get_value(runtime, section, key, value, value_size);
  }
  if (value_size == nullptr) return false;

  // Old ReShade headers predate the documented nullptr size-query contract.
  // Emulate it with a bounded temporary buffer so current MFGAmpereUnlock code
  // can use the modern two-call pattern without depending on undocumented ABI.
  std::array<char, 4096> scratch{};
  size_t length = scratch.size() - 1;
  if (!config_get_value(runtime, section, key, scratch.data(), &length)) return false;
  *value_size = length + 1;
  return true;
}

template <typename T>
inline bool get_config_value(api::effect_runtime* runtime, const char* section,
                             const char* key, T& value) {
  return config_get_value(runtime, section, key, value);
}

inline void set_config_value(api::effect_runtime* runtime, const char* section,
                             const char* key, const char* value) {
  config_set_value(runtime, section, key, value);
}

template <typename T>
inline void set_config_value(api::effect_runtime* runtime, const char* section,
                             const char* key, const T& value) {
  config_set_value(runtime, section, key, value);
}

}  // namespace reshade
#endif
