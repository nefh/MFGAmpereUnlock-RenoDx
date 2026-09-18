#include <cstdlib>
#include <iostream>

#include "../../../src/addons/mfgunlock/diagnostic_bridge.hpp"
#include "../../../src/addons/mfgdiagnostics/timeline.hpp"

namespace {
using namespace mfgdiagnostics::timeline;
using namespace mfgunlock::diagnostic;
unsigned int g_checks = 0;

void Check(bool condition, const char* reason) {
  ++g_checks;
  if (condition) return;
  std::cerr << "FAILED: " << reason << '\n';
  std::exit(1);
}

EvaluateEvent Generated(uint32_t index, uint32_t count) {
  EvaluateEvent event;
  event.phase = EvaluatePhase::kBegin;
  event.evaluation_id = 10 + index;
  event.generated_count_known = 1;
  event.generated_count = count;
  event.generated_index_known = 1;
  event.generated_index = index;
  event.reset_known = 1;
  event.reset = 0;
  event.automode_reset_known = 1;
  event.automode_reset = 0;
  auto& output = event.resources[static_cast<size_t>(ResourceKey::kOutputInterpolated)];
  output.key = static_cast<uint32_t>(ResourceKey::kOutputInterpolated);
  output.known = 1;
  output.object = 0x1234;
  return event;
}
}  // namespace

int main() {
  EvaluateEvent empty;
  Check(Classify(empty).kind == FrameKind::kUnknown, "missing metadata stays unknown");

  auto generated = Generated(1, 3);
  const auto classified = Classify(generated);
  Check(classified.kind == FrameKind::kGenerated && classified.generated_index == 1 &&
            classified.generated_count == 3,
        "confirmed provider output classifies generated");
  Check(MarkerLabel(classified) == "FG 1/3", "generated label");

  generated.reset_known = 1;
  generated.reset = 1;
  Check(Classify(generated).kind == FrameKind::kReset, "input reset wins over generated metadata");
  Check(MarkerLabel(Classify(generated)).empty(), "reset has no marker");
  auto automode_reset = Generated(1, 3);
  automode_reset.automode_reset = 1;
  Check(Classify(automode_reset).kind == FrameKind::kReset,
        "automode reset wins over generated metadata");
  auto unknown_reset = Generated(1, 3);
  unknown_reset.automode_reset_known = 0;
  Check(Classify(unknown_reset).kind == FrameKind::kUnknown,
        "unknown automode reset fails closed");

  auto missing_output = Generated(2, 3);
  missing_output.resources[static_cast<size_t>(ResourceKey::kOutputInterpolated)].known = 0;
  Check(Classify(missing_output).kind == FrameKind::kUnknown,
        "missing interpolated output fails closed");

  auto invalid_index = Generated(4, 3);
  Check(Classify(invalid_index).kind == FrameKind::kUnknown, "out of range index fails closed");

  const Classification source{FrameKind::kSource, 0, 0};
  Check(MarkerLabel(source).empty(), "source has no generated marker");
  Check(std::string(FrameKindName(FrameKind::kSource)) == "source", "source name retained");

  const auto dynamic_one = Classify(Generated(1, 1));
  const auto dynamic_three = Classify(Generated(2, 3));
  Check(MarkerLabel(dynamic_one) == "FG 1/1" && MarkerLabel(dynamic_three) == "FG 2/3",
        "dynamic counts are per evaluation without stale state");
  Check(Classify(empty).kind == FrameKind::kUnknown,
        "valid prior evaluation does not leak into missing event");

  OneShotCapture capture;
  capture.Arm();
  Check(capture.Armed() && !capture.Complete(), "one-shot arms");
  EvaluateEvent end = Generated(1, 3);
  end.phase = EvaluatePhase::kEnd;
  Check(!capture.Observe(end), "end before begin ignored");
  EvaluateEvent begin = Generated(1, 3);
  begin.evaluation_id = 77;
  Check(capture.Observe(begin), "first begin captured");
  EvaluateEvent second_begin = Generated(2, 3);
  second_begin.evaluation_id = 78;
  Check(!capture.Observe(second_begin), "second begin cannot replace armed capture");
  EvaluateEvent wrong_end = begin;
  wrong_end.phase = EvaluatePhase::kEnd;
  wrong_end.evaluation_id = 99;
  Check(!capture.Observe(wrong_end), "wrong evaluation end ignored");
  EvaluateEvent matching_end = begin;
  matching_end.phase = EvaluatePhase::kEnd;
  matching_end.result = 1;
  Check(capture.Observe(matching_end) && !capture.Armed() && capture.Complete(),
        "matching end completes one-shot");
  EvaluateEvent copied_begin{}, copied_end{};
  Check(capture.Snapshot(copied_begin, copied_end) && copied_begin.evaluation_id == 77 &&
            copied_end.result == 1,
        "completed capture snapshots exact pair");
  capture.Cancel();
  Check(!capture.Complete() && !capture.Armed(), "capture cancel clears state");

  std::cout << "diagnostics timeline: " << g_checks << " checks PASS\n";
  return 0;
}
