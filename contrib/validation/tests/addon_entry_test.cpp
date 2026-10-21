// SPDX-License-Identifier: MIT
#include <cassert>
#include <iostream>
#include <stdexcept>
#include "../../../src/addons/mfgunlock/addon_entry.hpp"

using mfgunlock::addonentry::Result;
using mfgunlock::addonentry::State;
static int self_object, host_object, other_host;
static void* const self = &self_object;
static void* const host = &host_object;
static void* const other = &other_host;
static unsigned checked = 0;
#define CHECK(x) do { assert(x); ++checked; } while (false)

int main() {
  using mfgunlock::addonentry::Succeeded;
  // Cold regular load: discover/register/setup once, followed by AddonInit.
  {
    State s; int resolves=0, registers=0, setups=0;
    auto resolve=[&]() { ++resolves; return host; };
    auto reg=[&](void* a, void* h) { ++registers; return a==self && h==host; };
    auto setup=[&]() { ++setups; };
    CHECK(s.Attach(self,nullptr,resolve,reg,setup)==Result::kReady);
    CHECK(s.Ready()); CHECK(s.Host()==host); CHECK(s.Attempts()==1);
    for(int i=0;i<100;++i)
      CHECK(s.Attach(self,host,resolve,reg,setup)==Result::kAlreadyReady);
    CHECK(resolves==1 && registers==1 && setups==1);
    CHECK(s.Attach(self,other,resolve,reg,setup)==Result::kWrongHost);
    CHECK(s.Attach(other,host,resolve,reg,setup)==Result::kWrongHost);
    CHECK(registers==1 && setups==1);
  }
  // Global Vulkan host not discoverable during early LoadFromDllMain. An
  // explicit host from the real manager retries without a successful early load.
  {
    State s; int resolves=0, registers=0, setups=0;
    auto resolve=[&]() -> void* { ++resolves; return nullptr; };
    auto reg=[&](void* a,void* h) { ++registers; return a==self && h==host; };
    auto setup=[&]() { ++setups; };
    CHECK(s.Attach(self,nullptr,resolve,reg,setup)==Result::kDeferred);
    CHECK(!s.Ready()); CHECK(registers==0 && setups==0);
    CHECK(s.Attach(self,host,resolve,reg,setup)==Result::kReady);
    CHECK(resolves==1 && registers==1 && setups==1);
    CHECK(s.Attempts()==2);
  }
  // A manager can initialize an already mapped but not registered addon.
  {
    State s; int registers=0, setups=0;
    auto resolve=[]() -> void* { assert(false); return nullptr; };
    CHECK(s.Attach(self,host,resolve,[&](void*,void*) {++registers;return true;},
                   [&]() {++setups;})==Result::kReady);
    CHECK(registers==1 && setups==1);
  }
  // API/ImGui/DisabledAddons rejection does not launch any runtime machinery.
  {
    State s; int setups=0;
    CHECK(s.Attach(self,host,[](){return host;},[](void*,void*){return false;},
                   [&](){++setups;})==Result::kRejected);
    CHECK(!s.Ready()); CHECK(setups==0);
    CHECK(s.Attach(self,host,[](){return host;},[](void*,void*){return true;},
                   [&](){++setups;})==Result::kReady);
    CHECK(setups==1);
  }
  // Loader-lock reentry must not wait/deadlock or register callbacks twice.
  {
    State s; int registers=0, setups=0;
    auto reg=[&](void*,void*) {
      ++registers;
      CHECK(s.Attach(self,host,[](){return host;},[](void*,void*){assert(false);return true;},
          [](){assert(false);})==Result::kBusy);
      return true;
    };
    CHECK(s.Attach(self,host,[](){return host;},reg,[&](){++setups;})==Result::kReady);
    CHECK(registers==1 && setups==1);
  }
  // Null addon must not be passed to the host registration function.
  {
    State s;
    CHECK(s.Attach(nullptr,host,[](){return host;},[](void*,void*){assert(false);return true;},
                   [](){assert(false);})==Result::kRejected);
    CHECK(!s.Ready());
  }
  // Both actual SDK source signatures: old one-argument with reference getter,
  // and modern two-argument without a writable reference.
  {
    void* cached=nullptr;
    auto getter=[&]() -> void*& {return cached;};
    auto reg=[&](void* a){return a==self && cached==host;};
    CHECK(mfgunlock::addonentry::RegisterWithHost(reg,getter,self,host));
    CHECK(cached==host);
    int getter_calls=0;
    auto modern_getter=[&](void* initial) -> void* {++getter_calls;return initial;};
    auto modern_reg=[](void* a,void* h){return a==self && h==host;};
    CHECK(mfgunlock::addonentry::RegisterWithHost(modern_reg,modern_getter,self,host));
    CHECK(getter_calls==0);
  }
  // D1 reproduction: registry/callbacks disappear while the addon and cached
  // handles survive. Recovery must not execute runtime setup a second time.
  {
    State s; int registrations=0, removals=0, callback_sets=0, runtime_setups=0;
    bool host_registered=false; int live_callbacks=0;
    auto reg=[&](void* a,void* h) {
      CHECK(a==self && h==host); CHECK(!host_registered);
      ++registrations; host_registered=true; return true;
    };
    auto callbacks=[&]() { CHECK(host_registered); ++callback_sets; ++live_callbacks; };
    auto setup=[&]() { ++runtime_setups; callbacks(); };
    CHECK(s.Attach(self,nullptr,[](){return host;},reg,setup)==Result::kReady);
    CHECK(s.Initialized() && s.Ready());
    auto replace=[&](void* a,void* h) {
      ++removals; host_registered=false; live_callbacks=0; return reg(a,h);
    };
    // The normal path refreshes an entry that still exists, without duplicates.
    CHECK(s.Refresh(self,host,replace,callbacks)==Result::kRefreshed);
    CHECK(host_registered && live_callbacks==1 && runtime_setups==1);
    for(int i=0;i<200;++i) {
      host_registered=false; live_callbacks=0; // host-manager recreation
      CHECK(s.Ready()); // historical success is NOT current host proof
      CHECK(s.Refresh(self,host,replace,callbacks)==Result::kRefreshed);
      CHECK(s.Ready() && s.Initialized() && host_registered);
      CHECK(live_callbacks==1 && runtime_setups==1);
    }
    CHECK(registrations==202 && removals==201 && callback_sets==202);
    CHECK(s.Attempts()==202);
  }
  // Failed refresh: no callbacks are bound, Ready is cleared, but existing
  // runtime ownership is retained for teardown and a later successful refresh.
  {
    State s; int setups=0,callbacks=0;
    CHECK(s.Attach(self,host,[](){return host;},[](void*,void*){return true;},
                   [&](){++setups;})==Result::kReady);
    CHECK(s.Refresh(self,host,[](void*,void*){return false;},
                    [&](){++callbacks;})==Result::kRejected);
    CHECK(!s.Ready() && s.Initialized() && setups==1 && callbacks==0);
    CHECK(s.Attach(self,host,[](){return host;},[](void*,void*){return true;},
                   [&](){++setups;})==Result::kRejected);
    CHECK(setups==1);
    CHECK(s.Refresh(self,host,[](void*,void*){return true;},
                    [&](){++callbacks;})==Result::kRefreshed);
    CHECK(s.Ready() && s.Initialized() && setups==1 && callbacks==1);
  }
  // Wrong live host/self never unregisters the valid binding, and refreshing a
  // cold addon never bypasses the required one-time initialization.
  {
    State s; int calls=0;
    auto replace=[&](void*,void*){++calls;return true;};
    auto callbacks=[&](){++calls;};
    CHECK(s.Refresh(self,host,replace,callbacks)==Result::kRejected);
    CHECK(calls==0 && !s.Initialized());
    CHECK(s.Attach(self,host,[](){return host;},[](void*,void*){return true;},[](){})==Result::kReady);
    CHECK(s.Refresh(self,other,replace,callbacks)==Result::kWrongHost);
    CHECK(s.Refresh(other,host,replace,callbacks)==Result::kWrongHost);
    CHECK(s.Refresh(nullptr,host,replace,callbacks)==Result::kWrongHost);
    CHECK(s.Refresh(self,nullptr,replace,callbacks)==Result::kWrongHost);
    CHECK(calls==0 && s.Ready() && s.Initialized());
  }
  // Reentry is nonblocking and cannot duplicate a registration/callback set.
  {
    State s; int callbacks=0;
    CHECK(s.Attach(self,host,[](){return host;},[](void*,void*){return true;},[](){})==Result::kReady);
    auto replace=[&](void*,void*){
      CHECK(s.Refresh(self,host,[](void*,void*){assert(false);return true;},
                      [](){assert(false);})==Result::kBusy);
      return true;
    };
    CHECK(s.Refresh(self,host,replace,[&](){++callbacks;})==Result::kRefreshed);
    CHECK(s.Ready() && s.Initialized() && callbacks==1);
  }
  CHECK(Succeeded(Result::kRefreshed));
  CHECK(Succeeded(Result::kReady)); CHECK(Succeeded(Result::kAlreadyReady));
  CHECK(!Succeeded(Result::kDeferred)); CHECK(!Succeeded(Result::kRejected));
  CHECK(!Succeeded(Result::kBusy)); CHECK(!Succeeded(Result::kWrongHost));
  std::cout << "addon_entry_test: " << checked << " checks PASS\n";
}
