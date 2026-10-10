#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile-test the production plugin image reader without mocking its body."""
import re
import sys
from pathlib import Path
source, output = map(Path, sys.argv[1:])
text = source.read_text(encoding='utf-8')
functions = []
for name in ('ModuleImage', 'FindFlipMeteringFallback', 'TryPatchFlipMeteringInModule'):
    matched = re.findall(r'^bool ' + name + r'\([^\{]+\{\n.*?^\}\n', text, re.M | re.S)
    if len(matched) != 1:
        raise SystemExit('expected exactly one production definition of ' + name)
    functions.extend(matched)
generated = '// Generated from addon.cpp; do not edit.\n' + '\n'.join(functions)
if not output.exists() or output.read_text(encoding='utf-8') != generated:
    output.write_text(generated, encoding='utf-8')
