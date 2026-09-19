#pragma once

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
