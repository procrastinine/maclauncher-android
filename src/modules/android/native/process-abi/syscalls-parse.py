"""Consume Bionic's original parser and register-width rules unchanged."""
import importlib.util
import json
import sys

spec = importlib.util.spec_from_file_location("bionic_gensyscalls", sys.argv[1])
original = importlib.util.module_from_spec(spec)
spec.loader.exec_module(original)
parser = original.SysCallsTxtParser()
parser.parse_file(sys.argv[2])
architecture = sys.argv[3]
assert architecture in ("arm", "arm64"), architecture
rows = []
for row in parser.syscalls:
    if not row.get(architecture):
        continue
    params = [p for p in row["params"] if p.strip() != "void"]
    registers = []
    cursor = 0
    for param in params:
        words = 2 if architecture == "arm" and original.param_uses_64bits(param) else 1
        if words == 2 and cursor % 2:
            cursor += 1
        registers.append([cursor, words])
        cursor += words
    assert cursor <= (7 if architecture == "arm" else 6), row
    rows.append({**row, "params": params, "registers": registers,
                 "macro": original.make__NR_name(row["name"])})
json.dump(rows, sys.stdout)
