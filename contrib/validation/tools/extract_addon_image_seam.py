#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile-test the production plugin image reader without mocking its body."""
import re
import sys
from pathlib import Path
source, output = map(Path, sys.argv[1:3])
transaction = len(sys.argv) == 4 and sys.argv[3] == '--transaction'
text = source.read_text(encoding='utf-8')
state = re.findall(r'^constexpr char kFlipMarker\[\].*?(?=^bool ModuleImage\()', text, re.M | re.S)
if len(state) != 1:
    raise SystemExit('expected exactly one production flip transaction section')
state = state[0]
if not transaction:
    # The offline corpus probe compiles the same scanner/cache but only plans
    # stores. Its own explicit stubs replace these two mutation entrypoints.
    for name in ('WriteFlipSite', 'RestoreFlipSitesFrom'):
        pattern = r'^bool ' + name + r'(\([^\{]+)\{\n.*?^\}\n'
        state, count = re.subn(pattern, lambda match: 'bool ' + name + match[1] + ';\n',
                               state, flags=re.M | re.S)
        if count != 1:
            raise SystemExit('expected exactly one production ' + name)
functions = [state]
for name in ('ModuleImage', 'FindFlipMeteringFallback', 'TryPatchFlipMeteringInModule',
             'TryPatchFlipMeteringOnce'):
    matched = re.findall(r'^bool ' + name + r'\([^\{]+\{\n.*?^\}\n', text, re.M | re.S)
    if len(matched) != 1:
        raise SystemExit('expected exactly one production definition of ' + name)
    functions.extend(matched)
if transaction:
    matched = re.findall(r'^void RestoreFlipMetering\([^\{]+\{\n.*?^\}\n', text, re.M | re.S)
    if len(matched) != 1:
        raise SystemExit('expected exactly one production RestoreFlipMetering')
    functions.extend(matched)
generated = '// Generated from addon.cpp; do not edit.\n' + '\n'.join(functions)
if not output.exists() or output.read_text(encoding='utf-8') != generated:
    output.write_text(generated, encoding='utf-8')
