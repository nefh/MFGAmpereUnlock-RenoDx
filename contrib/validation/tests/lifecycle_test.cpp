// SPDX-License-Identifier: MIT
// Production redirect/rollback logic with deterministic memory failures.
#include "../../../src/addons/mfgunlock/blackwell_temporal.hpp"
#include "../../../src/addons/mfgunlock/midpoint.hpp"
#include <cstdio>
#include <stdexcept>

namespace warp = mfgunlock::validatedwarp;
namespace temporal = mfgunlock::blackwelltemporal;
namespace mock = provider_mock;
namespace {
unsigned int g_checks = 0;
void Check(bool ok, const char* message) {
  ++g_checks;
  if (!ok) throw std::runtime_error(message);
}
struct Fixture {
  alignas(8) std::array<uint64_t, 16> image{};
  Fixture() {
    image.fill(0x12345678);
    mock::regions.push_back({image.data(), sizeof(image)});
  }
  ~Fixture() {
    std::erase_if(mock::regions, [this](const auto& r) { return r.base == image.data(); });
  }
  warp::internal::PreparedRedirect Prepare(size_t index) {
    warp::internal::PreparedRedirect prepared;
    prepared.module = image.data();
    prepared.located.address = reinterpret_cast<uint8_t*>(image.data());
    prepared.located.size = 16;
    prepared.expected = image[index];
    prepared.slots = {&image[index]};
    prepared.rebuilt.resize(80);
    return prepared;
  }
};
void Reset() {
  Check(mock::allocations.empty(), "no leaked fixture allocations");
  mock::protection_calls = mock::fail_protection_call = mock::frees = 0;
  mock::fail_protection_calls.clear();
  mock::fail_free = false;
}
}
int main() {
  try {
    for (unsigned failure : {1u, 2u}) {
      Reset();
      Fixture fixture;
      auto prepared = fixture.Prepare(0);
      std::vector<warp::Redirect> redirects(1);
      std::string detail;
      Check(warp::internal::CommitPreparedRedirect(prepared, "test", redirects[0], detail), "redirect commits");
      void* replacement = redirects[0].allocation;
      mock::protection_calls = 0;
      mock::fail_protection_call = failure;
      Check(!warp::Restore(redirects), "restore reports protection failure");
      Check(mock::frees == 0 && mock::allocations.contains(replacement) && !redirects.empty(),
            "failed restore retains memory and rollback metadata");
      mock::fail_protection_call = 0;
      Check(warp::Restore(redirects) && redirects.empty(), "later restore can retry");
      Check(fixture.image[0] == 0x12345678 && mock::frees == 1,
            "replacement freed only after complete restore");
      Check(mock::regions.front().protect == PAGE_READONLY, "original protection restored on retry");
    }
    for (uint32_t sm : {86u, 75u}) {
      Reset();
      Fixture fixture;
      temporal::Plan plan;
      plan.target_sm = sm;
      for (size_t i = 0; i < plan.kernels.size(); ++i) plan.kernels[i].redirect = fixture.Prepare(i);
      plan.ready = true;
      temporal::Result result;
      std::vector<warp::Redirect> redirects;
      Check(temporal::Commit(plan, redirects, result) && result.applied, "target plan commits transactionally");
      Check(result.detail.find("sm_" + std::to_string(sm)) != std::string::npos,
            "label reports the prepared target, not an assumed Ampere target");
      Check(temporal::Restore(redirects) && redirects.empty(), "target plan restores");
    }
    Reset();
    {
      Fixture fixture;
      temporal::Plan plan;
      for (size_t i = 0; i < plan.kernels.size(); ++i) plan.kernels[i].redirect = fixture.Prepare(i);
      plan.ready = true;
      temporal::Result result;
      std::vector<warp::Redirect> redirects;
      // First role commits (calls 1,2). Second role fails (3). Restore of
      // the first role also fails (4): midpoint must not compete with it.
      mock::fail_protection_calls = {3, 4};
      Check(!temporal::Commit(plan, redirects, result), "later role commit fails");
      Check(!result.fallback_safe && !redirects.empty(), "previous-role restore failure blocks fallback");
      Check(result.detail.find("fallback blocked") != std::string::npos, "unsafe rollback is reported");
      Check(mock::allocations.size() == 1 && fixture.image[0] != 0x12345678,
            "still-referenced earlier allocation retained");
      mock::fail_protection_calls.clear();
      Check(temporal::Restore(redirects), "failed temporal plan can restore later");
    }
    Reset();
    {
      Fixture fixture;
      auto prepared = fixture.Prepare(0);
      warp::Redirect redirect;
      std::string detail;
      mock::fail_protection_calls = {2, 3};
      Check(!warp::internal::CommitPreparedRedirect(prepared, "test", redirect, detail), "partial commit fails");
      Check(redirect.allocation && !redirect.descriptors.empty() && mock::frees == 0,
            "failed in-role rollback transfers ownership instead of losing it");
      std::vector<warp::Redirect> pending;
      pending.push_back(std::move(redirect));
      mock::fail_protection_calls.clear();
      Check(warp::Restore(pending), "in-role rollback retry succeeds");
    }
    Reset();
    {
      Fixture fixture;
      void* allocation = VirtualAlloc(nullptr, 80, MEM_COMMIT, PAGE_READWRITE);
      std::vector<mfgunlock::midpoint::Patch> patches{{&fixture.image[0], fixture.image[0]}};
      fixture.image[0] = reinterpret_cast<uint64_t>(allocation);
      mock::fail_protection_call = 1;
      Check(!mfgunlock::midpoint::Restore(patches, allocation), "midpoint restore propagates failure");
      Check(allocation && !patches.empty() && mock::frees == 0, "midpoint failed restore retains allocation");
      mock::fail_protection_call = 0;
      Check(mfgunlock::midpoint::Restore(patches, allocation) && !allocation, "midpoint retry restores");
    }
    Reset();
    {
      Fixture fixture;
      std::vector<mfgunlock::midpoint::Patch> patches;
      void* allocation = nullptr;
      std::string detail;
      mock::fail_protection_calls = {3, 4};
      Check(!mfgunlock::midpoint::RedirectDescriptors(
                {&fixture.image[0], &fixture.image[1]}, std::vector<uint8_t>(80),
                patches, allocation, detail), "midpoint partial redirect is not accepted");
      Check(allocation && patches.size() == 1 && mock::frees == 0 &&
                detail.find("rollback incomplete") != std::string::npos,
            "midpoint incomplete rollback retains allocation and reports failure");
      mock::fail_protection_calls.clear();
      Check(mfgunlock::midpoint::Restore(patches, allocation), "midpoint partial rollback can retry");
    }
    Reset();
    {
      std::array<uint8_t, 512> bytes{};
      mock::regions.push_back({bytes.data(), bytes.size()});
      auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(bytes.data());
      dos->e_magic = IMAGE_DOS_SIGNATURE;
      dos->e_lfanew = 0x7fffffff;
      std::string version, detail;
      Check(!warp::IsSupportedProvider(bytes.data(), version, detail),
            "Warp rejects invalid NT-header offset before dereference");
      std::vector<mfgunlock::midpoint::Patch> patches;
      void* allocation = nullptr;
      Check(!mfgunlock::midpoint::Apply(bytes.data(), patches, allocation, detail),
            "midpoint rejects invalid image before descriptor scan");
      mock::regions.pop_back();
    }
    Reset();
    std::printf("PASS lifecycle: %u checks\n", g_checks);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL lifecycle: %s\n", error.what());
    return 1;
  }
}
