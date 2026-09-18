// SPDX-License-Identifier: MIT
// Test ABI double. Production includes NVIDIA's SDK declaration instead.
#pragma once
#define NVSDK_CONV
struct NVSDK_NGX_Parameter {};
enum NVSDK_NGX_Result { NVSDK_NGX_Result_Success = 1, NVSDK_NGX_Result_FAIL = 0xbad00001u };
using PFN_NVSDK_NGX_Parameter_SetUI = void (*)(NVSDK_NGX_Parameter*, const char*, unsigned int);
using PFN_NVSDK_NGX_Parameter_SetI = void (*)(NVSDK_NGX_Parameter*, const char*, int);
using PFN_NVSDK_NGX_Parameter_GetUI = NVSDK_NGX_Result (*)(NVSDK_NGX_Parameter*, const char*, unsigned int*);
using PFN_NVSDK_NGX_Parameter_GetI = NVSDK_NGX_Result (*)(NVSDK_NGX_Parameter*, const char*, int*);
