#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile the production liveness poll and Present maintenance decision."""
import re
import sys
from pathlib import Path
source, output = map(Path, sys.argv[1:])
s = source.read_text(encoding='utf-8')
poll = re.findall(r'^void PollRuntimeModuleChanges\([^\{]+\{\n.*?^\}\n', s, re.M|re.S)
if len(poll) != 1:
    raise SystemExit('expected exactly one production liveness poll')
start = s.index('void OnPresentUiOutput(')
body = s.index(') {', start) + 3
end = s.index('  if (swapchain == nullptr) return;', body)
generated = '// Generated from addon.cpp: actual poll and Present decision.\n'+poll[0]+'\nvoid ProductionPresentMaintenanceTick() {'+s[body:end]+'}\n'
if not output.exists() or output.read_text() != generated:
    output.write_text(generated, encoding='utf-8')
