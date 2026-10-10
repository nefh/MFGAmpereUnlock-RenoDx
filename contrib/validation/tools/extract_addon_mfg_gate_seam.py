#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile exact production gate entry/restore and readiness lambda, no copies."""
import re
import sys
from pathlib import Path
src, out = map(Path, sys.argv[1:])
s = src.read_text(encoding='utf-8')
parts = []
for name in ('PatchArchGatesInModule', 'RestoreDlssgArchGate'):
    found = re.findall(r'^void ' + name + r'\([^\{]*\{\n.*?^\}\n', s, re.M | re.S)
    if len(found) != 1:
        raise SystemExit('expected exactly one production ' + name)
    parts.append(found[0])
found = re.findall(r'mfgunlock::framecount::g_multi_frame_ready = \[\]\(\) \{\n(.*?)^      \};', s, re.M | re.S)
if len(found) != 1:
    raise SystemExit('expected production multi-frame readiness lambda')
parts.append('bool ProductionMultiFrameReady() {\n' + found[0] + '}\n')
text = '// Extracted from current addon.cpp; do not edit.\n' + '\n'.join(parts)
if not out.exists() or out.read_text() != text:
    out.write_text(text, encoding='utf-8')
