#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Extract exact addon functions for the existing portable test mechanism."""
import re
import sys
from pathlib import Path
source, output = map(Path, sys.argv[1:])
s = source.read_text(encoding="utf-8")
parts = []
for name in ("ModuleImage", "ObserveFrameCountCeiling", "PatchFrameCountCeiling",
             "RestoreFrameCountCeiling", "OnDlssgPluginUnloaded", "ReserveDebugRows", "DrawWarpStatus"):
    matches = re.findall(r"^(?:void|bool) " + name + r"\([^;\{]*\{\n.*?^\}\n", s, re.M | re.S)
    if len(matches) != 1:
        raise SystemExit("expected exact production function: " + name)
    parts.append(matches[0])
a = s.index('  // A failed try-lock is not a change of feature state.')
b = s.index('  ImGui::TextUnformatted(isr_status.c_str());', a) + len('  ImGui::TextUnformatted(isr_status.c_str());')
parts.append('void ProductionDrawIsr(bool scatter) {\n'+s[a:b]+'}\n')
a = s.index('    static std::string temporal_status =')
b = s.index('    ImGui::TextUnformatted(temporal_status.c_str());', a) + len('    ImGui::TextUnformatted(temporal_status.c_str());')
parts.append('void ProductionDrawTemporal(bool temporal) {\n'+s[a:b]+'}\n')
data = '// Generated from actual addon.cpp, not an independent implementation.\n'+'\n'.join(parts)
if not output.exists() or output.read_text() != data:
    output.write_text(data, encoding="utf-8")
