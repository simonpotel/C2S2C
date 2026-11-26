"""The generated Python registry must match Generated/registry.txt.

Why: see Tests/Generated/C/test_generated.c. Usage: test_generated.py <path to registry.txt>
"""
import sys
from pathlib import Path

# The generated module lives in Generated/Python (two levels above Tests/Generated/Python).
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "Generated/Python"))
import c2s2c_registry as reg  # noqa: E402

passed = failed = 0
lines = Path(sys.argv[1]).read_text().split("\n")
expected = {}
for line in filter(None, lines):
    pid, name, schema = line.split()
    expected[int(pid, 16)] = (name, schema)
    # Each packet: the table entry is right AND the module constant NAME equals the id.
    ok = reg.PACKETS.get(int(pid, 16)) == (name, schema) and getattr(reg, name.upper()) == int(pid, 16)
    passed, failed = passed + ok, failed + (not ok)

# Nothing extra in the table.
ok = len(reg.PACKETS) == len(expected)
passed, failed = passed + ok, failed + (not ok)
print(f"{passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
