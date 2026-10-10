// SPDX-License-Identifier: MIT
// C1 test-only candidate. Not enabled by the production addon.
inline Snapshot CandidateRead(uint64_t now = NowNs()) {
  Snapshot result{};
  std::array<Ring,kCpuKinds> cpu;
  std::array<uint32_t,kCpuKinds> warmup;
  Ring source, native;
  {
    std::lock_guard guard(g_lock);
    result.enabled = g_enabled.load(); result.generation = g_generation.load();
    result.timestamp_ns = now; result.lost_events = g_lost.load(); result.invalid_events = g_invalid;
    result.native_viewport = g_native_viewport; result.native_viewport_conflict = g_native_conflict;
    // C1: disabled snapshots expose metadata only; avoid copying the rings.
    if (!result.enabled) return result;
    cpu = g_cpu; warmup = g_warmup; source = g_source; native = g_native;
  }
  // All filtering and sorting outside the tap lock and outside the frame hooks.
  result.source_callbacks = Rate(source, now);
  result.native_presented = Rate(native, now);
  for (size_t kind=0; kind<kCpuKinds; ++kind) {
    const auto& ring = cpu[kind]; auto& stats = result.cpu[kind];
    stats.total = ring.total; stats.warmup = warmup[kind];
    stats.overwritten = ring.total > kCapacity ? ring.total - kCapacity : 0;
    std::array<uint64_t,kCapacity> sorted{};
    for(uint32_t i=0;i<ring.size;++i) {
      const auto& value=ring.samples[i];
      if (!Fresh(now,value.at)) continue;
      sorted[stats.retained++] = value.value;
      if (!stats.first_ns || value.at < stats.first_ns) stats.first_ns = value.at;
      stats.last_ns = (std::max)(stats.last_ns, value.at);
    }
    if (stats.last_ns && now >= stats.last_ns) stats.age_ns = now - stats.last_ns;
    if (!stats.retained) continue;
    std::sort(sorted.begin(), sorted.begin()+static_cast<size_t>(stats.retained));
    const auto quantile=[&](uint64_t percent) { return sorted[(stats.retained*percent+99)/100-1]; };
    stats.p50_ns=quantile(50);stats.p95_ns=quantile(95);stats.p99_ns=quantile(99);stats.valid=1;
  }
  return result;
}
