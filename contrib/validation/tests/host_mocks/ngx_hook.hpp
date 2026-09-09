// Does not install machine-code detours. Tests trampoline selection only.
#pragma once
#include "mock_api.hpp"
namespace mfgunlock::hook {
using HookItem=std::tuple<const char*,void**,void*>;
inline bool Install(HMODULE module,const std::vector<HookItem>& hooks,const char*) {
  ++mock::installs;
  if(!mock::install_ok)return false;
  for(const auto& [name,storage,replacement]:hooks){
    (void)replacement;
    if(*storage||!GetProcAddress(module,name))return false;
  }
  for(const auto& [name,storage,replacement]:hooks){
    (void)replacement;
    *storage=reinterpret_cast<void*>(GetProcAddress(module,name));
  }
  return true;
}
}
