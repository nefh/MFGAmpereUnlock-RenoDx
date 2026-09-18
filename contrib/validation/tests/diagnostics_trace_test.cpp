// SPDX-License-Identifier: MIT
#include <cstdint>
#include <iostream>

#include "../../../src/addons/mfgdiagnostics/trace.hpp"

namespace {
int g_failures = 0;
void Check(bool condition, const char* message) {
  if (condition) return;
  ++g_failures;
  std::cerr << "FAIL: " << message << '\n';
}
}

int main() {
  mfgdiagnostics::Capture<3> capture;
  capture.Start(100, 1000);
  const uint64_t ticket = capture.Ticket(100);
  Check(ticket != 0, "capture ticket is active");

  for (int i = 0; i < 2; ++i) {
    mfgdiagnostics::Event event(mfgdiagnostics::Kind::evaluate);
    event.generation = ticket;
    capture.Submit(event);
  }
  Check(capture.events.size() == 2, "two events retained");
  Check(capture.events[0].sequence == 1, "first automatic sequence is one");
  Check(capture.events[1].sequence == 2, "second automatic sequence is two");

  mfgdiagnostics::Event explicit_sequence(mfgdiagnostics::Kind::frame_count);
  explicit_sequence.generation = ticket;
  explicit_sequence.sequence = 77;
  capture.Submit(explicit_sequence);
  Check(capture.events.size() == 3, "explicit-sequence event retained");
  Check(capture.events[2].sequence == 77, "explicit sequence is preserved");

  mfgdiagnostics::Event overflow(mfgdiagnostics::Kind::tag);
  overflow.generation = ticket;
  capture.Submit(overflow);
  Check(capture.full.load(), "capacity overflow is explicit");
  Check(capture.active.load() == 0, "full capture stops accepting events");

  capture.Start(200, 10);
  Check(capture.events.empty(), "restart clears events");
  Check(capture.Ticket(211) == 0, "deadline expires capture");

  namespace dg = mfgunlock::diagnostic;
  mfgdiagnostics::Capture<8> lifecycle;
  lifecycle.Start(300, 100);
  const std::array kinds{dg::LifecycleKind::kQualityRequested, dg::LifecycleKind::kModeOff,
      dg::LifecycleKind::kReleased, dg::LifecycleKind::kZeroHandles,
      dg::LifecycleKind::kCreated, dg::LifecycleKind::kQualityRestartRequired};
  for (auto kind : kinds) {
    dg::LifecycleEvent source;
    source.kind = kind;
    source.epoch = 2;
    source.handles = kind == dg::LifecycleKind::kZeroHandles ? 0 : 1;
    auto event = mfgdiagnostics::SnapshotLifecycle(source);
    Check(event.recognized && event.values[7] == mfgdiagnostics::kUnknown,
          "unknown tracking confidence is not encoded as false");
    event.generation = lifecycle.Ticket(301);
    lifecycle.Submit(event);
  }
  Check(lifecycle.events.size() == kinds.size(), "quality and lifecycle use the same bounded trace");
  for (size_t i = 0; i < kinds.size(); ++i)
    Check(lifecycle.events[i].values[0] == static_cast<uint32_t>(kinds[i]) &&
          lifecycle.events[i].sequence == i + 1, "lifecycle sequence preserved without a fake reconfigure commit");
  dg::LifecycleEvent malformed;
  malformed.version = 99;
  Check(!mfgdiagnostics::SnapshotLifecycle(malformed).recognized, "unknown lifecycle ABI rejected");
  malformed.version = dg::kLifecycleVersion;
  malformed.bytes = 4;
  Check(!mfgdiagnostics::SnapshotLifecycle(malformed).recognized, "short lifecycle payload rejected");

  if (g_failures != 0) return 1;
  std::cout << "diagnostics_trace_test=PASS\n";
  return 0;
}
