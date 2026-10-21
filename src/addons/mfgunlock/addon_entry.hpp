// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <type_traits>
#include <utility>

namespace mfgunlock::addonentry {

// Only manages registration/callback setup. Provider preparation and runtime
// lifecycle remain in the existing bootstrap. No waits under the loader lock.
enum class Result { kReady, kAlreadyReady, kDeferred, kRejected, kBusy, kWrongHost, kRefreshed };

// ReShade's header-only SDK caches function pointers. The addon also owns
// hooks/workers that outlive temporary Vulkan instances. Retain the exact
// host image before the first SDK call; never rebind cached pointers by name.
// The platform callback pins the already-loaded module until process exit.
class HostLifetime {
 public:
  template <class Pin>
  bool Ensure(void* host, Pin&& pin) {
    if (!host) return false;
    if (void* current = host_.load(std::memory_order_acquire)) return current == host;
    if (!pin(host)) return false;
    void* expected = nullptr;
    return host_.compare_exchange_strong(expected, host, std::memory_order_acq_rel) ||
        expected == host;
  }
  void* Host() const { return host_.load(std::memory_order_acquire); }
 private:
  std::atomic<void*> host_{nullptr};
};

class State {
 public:
  template <class Resolve, class Register, class Setup>
  Result Attach(void* self, void* requested_host, Resolve&& resolve,
                Register&& register_addon, Setup&& setup) {
    if (busy_.test_and_set(std::memory_order_acquire)) return Result::kBusy;
    struct Release {
      std::atomic_flag& flag;
      ~Release() { flag.clear(std::memory_order_release); }
    } release{busy_};

    if (ready_.load(std::memory_order_acquire)) {
      // Header-only ReShade APIs cache their host function pointers. Do not
      // pretend those can be rebound to a different live ReShade instance.
      return self == self_.load(std::memory_order_relaxed) &&
          (!requested_host || requested_host == host_.load(std::memory_order_relaxed))
          ? Result::kAlreadyReady : Result::kWrongHost;
    }
    // A failed refresh may leave initialized hooks/workers but no host binding.
    // Only Refresh may recover that state, without repeating setup.
    if (initialized_.load(std::memory_order_acquire)) return Result::kRejected;
    ++attempts_;
    void* const host = requested_host ? requested_host : resolve();
    if (!host) return Result::kDeferred;
    if (!self || !register_addon(self, host)) return Result::kRejected;

    setup();
    initialized_.store(true, std::memory_order_release);
    self_ = self;
    host_ = host;
    ready_.store(true, std::memory_order_release);
    return Result::kReady;
  }

  // AddonInit is a new host-manager initialization boundary. A successful
  // earlier DllMain call is not proof that the host still owns our registry
  // entry or callbacks (the module can survive a host/layer reinitialization).
  // Refresh ONLY the ReShade registration. Never repeat runtime setup/workers.
  template <class ReplaceRegistration, class Callbacks>
  Result Refresh(void* self, void* requested_host,
                 ReplaceRegistration&& replace_registration,
                 Callbacks&& callbacks) {
    if (busy_.test_and_set(std::memory_order_acquire)) return Result::kBusy;
    struct Release {
      std::atomic_flag& flag;
      ~Release() { flag.clear(std::memory_order_release); }
    } release{busy_};

    if (!initialized_.load(std::memory_order_acquire)) return Result::kRejected;
    if (!self || self != self_.load(std::memory_order_relaxed) ||
        !requested_host || requested_host != host_.load(std::memory_order_relaxed))
      return Result::kWrongHost;

    ++attempts_;
    ready_.store(false, std::memory_order_release);
    if (!replace_registration(self, requested_host)) return Result::kRejected;
    callbacks();
    ready_.store(true, std::memory_order_release);
    return Result::kRefreshed;
  }

  // Teardown still has to stop the runtime after a failed registration refresh.
  // 'Ready' describes only the last host registration, not ownership of hooks.
  bool Initialized() const { return initialized_.load(std::memory_order_acquire); }
  bool Ready() const { return ready_.load(std::memory_order_acquire); }
  void* Host() const { return host_.load(std::memory_order_acquire); }
  unsigned Attempts() const { return attempts_.load(std::memory_order_relaxed); }

 private:
  std::atomic_flag busy_ = ATOMIC_FLAG_INIT;
  std::atomic<void*> self_{nullptr};
  std::atomic<void*> host_{nullptr};
  std::atomic_uint attempts_{0};
  std::atomic_bool ready_{false};
  std::atomic_bool initialized_{false};
};

inline bool Succeeded(Result result) {
  return result == Result::kReady || result == Result::kAlreadyReady ||
      result == Result::kRefreshed;
}

inline const char* Name(Result result) {
  switch (result) {
    case Result::kReady: return "READY";
    case Result::kAlreadyReady: return "ALREADY_READY";
    case Result::kDeferred: return "DEFERRED_NO_HOST";
    case Result::kRejected: return "REGISTRATION_REJECTED";
    case Result::kBusy: return "INITIALIZATION_IN_PROGRESS";
    case Result::kWrongHost: return "HOST_CHANGED";
    case Result::kRefreshed: return "HOST_REGISTRATION_REFRESHED";
  }
  return "INVALID_STATE";
}

// Old SDKs return their cached host by reference and register_addon accepts one
// argument. New SDKs provide register_addon(addon, host). Use the real signature,
// not a guessed SDK-version threshold; both retain the SDK's ABI/ImGui checks.
template <class Register, class HostGetter, class Module>
bool RegisterWithHost(Register register_addon, HostGetter get_host,
                      Module self, Module host) {
  if constexpr (std::is_invocable_r_v<bool, Register, Module, Module>) {
    return register_addon(self, host);
  } else {
    static_assert(std::is_lvalue_reference_v<decltype(get_host())>,
                  "Legacy ReShade SDK must expose a writable cached host");
    get_host() = host;
    return register_addon(self);
  }
}

}  // namespace mfgunlock::addonentry
