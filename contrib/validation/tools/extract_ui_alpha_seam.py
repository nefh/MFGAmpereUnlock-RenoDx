#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile exact production alpha callbacks, not a parallel lifetime model."""
import hashlib
import re
import sys
from pathlib import Path
addon, framecount, out = map(Path, sys.argv[1:])
a, f = addon.read_text(encoding="utf-8-sig"), framecount.read_text(encoding="utf-8-sig")
parts = []
for name in ('UiCandidateSnapshot', 'UiAlphaCandidateSnapshot'):
    matches = re.findall(r'^struct '+name+r' \{\n.*?^\};', f, re.M | re.S)
    if len(matches) != 1:
        raise SystemExit('missing/ambiguous production '+name)
    parts.append(matches[0])
structs = 'namespace mfgunlock::framecount {\n'+'\n'.join(parts)+'\n}\n'
start = a.index('constexpr size_t kUiAlphaSlots =')
end = a.index('\n#endif', start)
globals_ = a[start:end]
counts = re.findall(r'^std::atomic<unsigned long long> g_ui_alpha_.*?;', a, re.M)
start = a.index('reshade::api::resource_usage UiResourceUsage(')
end = a.index('\n#else\nvoid DestroyUiAlphaBackend', start)
body = a[start:end]
finish = re.findall(r'^void OnFinishPresentUiAlpha\(.*?^\}', a, re.M | re.S)
if len(finish) != 1: raise SystemExit('missing production Present callback')
payload = ('// Generated directly from addon.cpp and framecount.hpp. Do not edit.\n'+
           '// addon SHA256 '+hashlib.sha256(addon.read_bytes()).hexdigest()+'\n'+
           structs + globals_ + '\n'+'\n'.join(counts)+'\n'+body+'\n'+finish[0]+'\n')
# declarations for callbacks supplied by candidate discovery; definitions in test
cut = payload.index('constexpr size_t kUiAlphaSlots')
payload = payload[:cut]+'''bool AcquireUiCandidate(const mfgunlock::framecount::UiCandidateSnapshot*);
void ReleaseUiCandidate(const mfgunlock::framecount::UiCandidateSnapshot*, bool, sl::Result);
void MarkUiCandidateInjected(const mfgunlock::framecount::UiCandidateSnapshot*);
'''+payload[cut:]
if not out.exists() or out.read_text(encoding="utf-8") != payload:
    out.write_text(payload, encoding="utf-8", newline="\n")
